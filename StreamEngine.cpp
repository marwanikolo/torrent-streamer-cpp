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
#include <iomanip>
#include <algorithm>

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
    
    write_debug_log(debug, std::format("[INIT] Selected File: {} ({} bytes)", selected_path, state.file_size));
    
    std::cout << "\n[*] Piece Size: " << (state.piece_length / 1024) << " KB | Total Pieces: " << state.num_pieces << "\n";

    std::vector<lt::download_priority_t> priorities(files.num_files(), lt::default_priority);
    h.prioritize_files(priorities);
    h.unset_flags(lt::torrent_flags::sequential_download);
    
    for (int p = 0; p < state.num_pieces; ++p) {
        h.piece_priority(lt::piece_index_t(p), lt::dont_download);
    }
    
    std::string playlist_path = state.file_path + ".m3u8";

    if (selected_path.length() >= 5 && selected_path.substr(selected_path.length() - 5) == ".m2ts") {
        if (file_exists(playlist_path)) {
            std::cout << "\n[*] Found cached HLS playlist on disk. Skipping index download...\n";
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
                
                std::cout << "\n[*] Downloading Blu-ray index (.clpi) for HLS generation...\n";
                
                std::vector<std::int64_t> fp;
                int retry_counter = 0;
                
                while(true) {
                    h.file_progress(fp);
                    
                    if (fp.size() > clpi_idx && fp[clpi_idx] >= files.file_size(lt::file_index_t(clpi_idx))) {
                        std::cout << "\n[*] Index downloaded successfully.\n";
                        break;
                    }
                    
                    if (interrupted.load()) {
                        std::cout << "\n[-] Stream aborted by user. Returning to menu...\n";
                        for (int p = 0; p < state.num_pieces; ++p) h.piece_priority(lt::piece_index_t(p), lt::dont_download);
                        h.clear_piece_deadlines();
                        interrupted = false; 
                        std::cin.clear();
                        return; 
                    }
                    
                    if (fp.size() > clpi_idx && files.file_size(lt::file_index_t(clpi_idx)) > 0) {
                        double clpi_prog = (static_cast<double>(fp[clpi_idx]) / static_cast<double>(files.file_size(lt::file_index_t(clpi_idx)))) * 100.0;
                        std::cout << "\r[>] Index Progress: " << std::fixed << std::setprecision(2) << clpi_prog << "%   " << std::flush;
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
                    std::cout << "[*] Parsing index and caching HLS playlist to disk...\n";
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
    } // <--- THIS WAS THE MISSING BRACE!

    h.piece_priority(lt::piece_index_t(state.first_piece), lt::top_priority);
    h.piece_priority(lt::piece_index_t(state.last_piece), lt::top_priority);

    std::thread alert_thread(alert_loop, std::ref(ses), &state, resume_path, debug);

    while (!file_exists(state.file_path)) {
        if (interrupted.load()) {
            std::cout << "\n[-] Stream aborted by user. Returning to menu...\n";
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
                             
    std::cout << "\n[*] Launching " << (hls_playlist.empty() ? "raw stream" : "HLS timeline stream") << "...\n";
    launch_player(config, launch_url);
    std::cout << "\n[!] STREAM ACTIVE: Press Ctrl+C to STOP and RETURN TO MENU.\n\n";

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

        std::cout << "\r\033[K[>] DL: " << (st.download_rate / 1000) << " kB/s | "
                  << std::fixed << std::setprecision(2) << actual_progress << "% | "
                  << "Peers: " << st.num_peers << " (TCP: " << standard_count << " / WebRTC: " << webrtc_count << ") | "
                  << "Tracked: " << active_pieces << std::flush;
                  
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::cout << "\n\n[*] Stopping player and returning to file menu...\n";
    
    stop_player();
    h.save_resume_data();
    
    state.shutting_down = true;
    state.cv.notify_all();
    
    svr.stop();
    
    std::thread([&config]() {
        httplib::Client cli("localhost", config.port);
        cli.set_connection_timeout(0, 100000); // 100ms
        cli.Get("/");
    }).detach();

    if (server_thread.joinable()) server_thread.join();
    if (alert_thread.joinable()) alert_thread.join();
    
    for (int p = 0; p < state.num_pieces; ++p) {
        h.piece_priority(lt::piece_index_t(p), lt::dont_download);
    }
    h.clear_piece_deadlines();

    interrupted = false; 
    std::cin.clear();
}
