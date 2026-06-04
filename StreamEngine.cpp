#include "StreamEngine.h"
#include "StreamState.h"
#include "AlertHandler.h"
#include "HttpServer.h"
#include "ProcessManager.h"
#include "MediaParser.h"
#include "Utils.h"
#include <libtorrent/torrent_status.hpp>
#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <format>
#include <print>     // <--- NEW C++23 Print Library
#include <algorithm>
// (Removed <iomanip> as C++23 native formatting handles precision natively)

extern std::atomic<bool> interrupted;

void stream_file(lt::session& ses, AppConfig& config, lt::torrent_handle& h, 
                 std::shared_ptr<const lt::torrent_info> ti, int choice, const std::string& resume_path) {
    
    interrupted = false; 

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    lt::file_storage const& files = ti->files();
#pragma GCC diagnostic pop

    StreamState state;
    state.h = h;
    
    state.file_path = config.save_dir + "/" + files.file_path(lt::file_index_t(choice));
    state.file_size = files.file_size(lt::file_index_t(choice));
    state.file_offset = files.file_offset(lt::file_index_t(choice));
    
    state.piece_length = ti->piece_length();
    state.num_pieces = ti->num_pieces();
    state.first_piece = state.file_offset / state.piece_length;
    state.last_piece = (state.file_offset + state.file_size - 1) / state.piece_length;

    std::string hls_playlist = "";
    std::string selected_path = files.file_path(lt::file_index_t(choice));
    bool debug = config.debug_mode;
    
    write_debug_log(debug, "[INIT] Selected File: {} ({} bytes)", selected_path, state.file_size);
    
    std::println("\n[*] Piece Size: {} KB | Total Pieces: {}", (state.piece_length / 1024), state.num_pieces);

    std::vector<lt::download_priority_t> priorities(files.num_files(), lt::default_priority);
    h.prioritize_files(priorities);
    h.unset_flags(lt::torrent_flags::sequential_download);
    
    for (int p = 0; p < state.num_pieces; ++p) {
        h.piece_priority(lt::piece_index_t(p), lt::dont_download);
    }
    
    std::string playlist_path = state.file_path + ".m3u8";

    if (selected_path.length() >= 5 && selected_path.substr(selected_path.length() - 5) == ".m2ts") {
        if (file_exists(playlist_path)) {
            std::println("\n[*] Found cached HLS playlist on disk. Skipping index download...");
            std::ifstream ifs(playlist_path);
            hls_playlist.assign((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));
        } 
        else {
            std::string clpi_path = selected_path;
            size_t pos = clpi_path.find("STREAM");
            if (pos != std::string::npos) clpi_path.replace(pos, 6, "CLIPINF");
            pos = clpi_path.find(".m2ts");
            if (pos != std::string::npos) clpi_path.replace(pos, 5, ".clpi");

            int clpi_idx = -1;
            for (int i = 0; i < files.num_files(); ++i) {
                if (files.file_path(lt::file_index_t(i)) == clpi_path) { clpi_idx = i; break; }
            }

            if (clpi_idx != -1) {
                priorities[clpi_idx] = lt::top_priority;
                h.prioritize_files(priorities);
                
                int clpi_first_p = files.file_offset(lt::file_index_t(clpi_idx)) / state.piece_length;
                int clpi_last_p = (files.file_offset(lt::file_index_t(clpi_idx)) + files.file_size(lt::file_index_t(clpi_idx)) - 1) / state.piece_length;
                
                std::println("\n[*] Downloading Blu-ray index (.clpi) for HLS generation...");
                
                std::vector<std::int64_t> fp;
                int retry_counter = 0;
                
                while(true) {
                    h.file_progress(fp);
                    
                    if (fp.size() > clpi_idx && fp[clpi_idx] >= files.file_size(lt::file_index_t(clpi_idx))) {
                        std::println("\n[*] Index downloaded successfully.");
                        break;
                    }
                    
                    if (interrupted.load()) {
                        std::println("\n[-] Stream aborted by user. Returning to menu...");
                        for (int p = 0; p < state.num_pieces; ++p) h.piece_priority(lt::piece_index_t(p), lt::dont_download);
                        h.clear_piece_deadlines();
                        interrupted = false; 
                        std::cin.clear();
                        return; 
                    }
                    
                    if (fp.size() > clpi_idx && files.file_size(lt::file_index_t(clpi_idx)) > 0) {
                        double clpi_prog = (static_cast<double>(fp[clpi_idx]) / static_cast<double>(files.file_size(lt::file_index_t(clpi_idx)))) * 100.0;
                        std::print("\r[>] Index Progress: {:.2f}%   ", clpi_prog);
                        std::fflush(stdout); // Required when printing to terminal without a newline
                    }
                    
                    if (++retry_counter % 25 == 0) { 
                        for (int p = clpi_first_p; p <= clpi_last_p; ++p) {
                            h.piece_priority(lt::piece_index_t(p), lt::top_priority);
                            h.set_piece_deadline(lt::piece_index_t(p), 1000); 
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }

                if (fp.size() > clpi_idx && fp[clpi_idx] >= files.file_size(lt::file_index_t(clpi_idx))) {
                    std::println("[*] Parsing index and caching HLS playlist to disk...");
                    std::string full_clpi = config.save_dir + "/" + files.file_path(lt::file_index_t(clpi_idx));
                    auto m_idx = parse_clpi_file(full_clpi);
                    
                    if (!m_idx.empty()) {
                        hls_playlist = generate_hls(m_idx, state.file_size, config.port);
                        
                        std::ofstream ofs(playlist_path);
                        if (ofs.is_open()) {
                            ofs << hls_playlist;
                            ofs.close();
                        } 
                    }
                }
            }
        }
    } 

    h.piece_priority(lt::piece_index_t(state.first_piece), lt::top_priority);
    h.piece_priority(lt::piece_index_t(state.last_piece), lt::top_priority);

    std::thread alert_thread(alert_loop, std::ref(ses), &state, resume_path, debug);

    while (!file_exists(state.file_path)) {
        if (interrupted.load()) {
            std::println("\n[-] Stream aborted by user. Returning to menu...");
            state.shutting_down = true;
            state.cv.notify_all();
            if (alert_thread.joinable()) alert_thread.join();
            for (int p = 0; p < state.num_pieces; ++p) h.piece_priority(lt::piece_index_t(p), lt::dont_download);
            h.clear_piece_deadlines();
            interrupted = false;
            std::cin.clear();
            return; 
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    httplib::Server svr;
    run_http_server(svr, state, hls_playlist, config);

    std::thread server_thread([&]() { svr.listen("localhost", config.port); });
    
    while (!svr.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    
    std::string launch_url = hls_playlist.empty() ? 
                             std::format("http://localhost:{}/stream", config.port) : 
                             std::format("http://localhost:{}/playlist.m3u8", config.port);
                             
    std::println("\n[*] Launching {}...", (hls_playlist.empty() ? "raw stream" : "HLS timeline stream"));
    launch_player(config, launch_url);
    std::println("\n[!] STREAM ACTIVE: Press Ctrl+C to STOP and RETURN TO MENU.\n");

    while (!interrupted.load()) {
        lt::torrent_status st = h.status();
        
        std::vector<std::int64_t> fp;
        h.file_progress(fp); 
        
        double actual_progress = 0.0;
        if (!fp.empty() && choice < fp.size() && state.file_size > 0) {
            actual_progress = (static_cast<double>(fp[choice]) / static_cast<double>(state.file_size)) * 100.0;
        }

        std::vector<lt::peer_info> peers;
        h.get_peer_info(peers);

        int webrtc_count = 0;
        int standard_count = 0;

        for (const auto& p : peers) {
            std::string client = p.client;
            std::transform(client.begin(), client.end(), client.begin(), ::tolower);
            
            if (client.find("web") != std::string::npos || client.find("brave") != std::string::npos) {
                webrtc_count++;
            } else {
                standard_count++;
            }
        }

        std::string active_pieces = "[";
        int count = 0;
        {
            std::lock_guard<std::mutex> lk(state.mtx);
            for (auto it = state.piece_refs.begin(); it != state.piece_refs.end(); ++it) {
                if (count++ >= 12) { active_pieces += " ..."; break; } 
                active_pieces += " " + std::to_string(it->first);
            }
        }
        active_pieces += " ]";

        // NEW C++23: std::print handles the complex formatting directly
        std::print("\r\033[K[>] DL: {} kB/s | {:.2f}% | Peers: {} (TCP: {} / WebRTC: {}) | Tracked: {}", 
                   (st.download_rate / 1000), actual_progress, st.num_peers, standard_count, webrtc_count, active_pieces);
        std::fflush(stdout);
                  
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::println("\n\n[*] Shutting down engine...");
    
    // 1. Lock out any new HTTP requests and wake up sleeping threads
    state.shutting_down = true;
    state.cv.notify_all();
    
    // 2. Stop the player so it stops asking for data
    stop_player();
    
    // 3. Gracefully stop the HTTP server
    svr.stop();
    std::thread([&config]() {
        httplib::Client cli("localhost", config.port);
        cli.set_connection_timeout(0, 100000); 
        cli.Get("/"); // Dummy request to unblock the server listen loop
    }).detach();

    if (server_thread.joinable()) server_thread.join();

    // 4. NOW it is 100% safe to pause the swarm and serialize to disk
    std::println("[*] Saving fastresume data...");
    h.pause();
    h.save_resume_data();
    
    // Wait for the alert loop to finish saving
    if (alert_thread.joinable()) alert_thread.join();
    
    // Cleanup piece priorities for the next stream
    for (int p = 0; p < state.num_pieces; ++p) {
        h.piece_priority(lt::piece_index_t(p), lt::dont_download);
    }
    h.clear_piece_deadlines();

    interrupted = false; 
    std::cin.clear();
}
