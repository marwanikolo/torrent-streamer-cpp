#include "TorrentEngine.h"
#include "ProcessManager.h"
#include "MediaParser.h"
#include <httplib.h>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/write_resume_data.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/settings_pack.hpp>

#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <regex>
#include <format>
#include <iomanip>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <map>
#include <memory>
#include <atomic>
#include <ctime>

// --- THREAD-SAFE DEBUG LOGGER ---
std::mutex g_log_mtx;
void write_debug_log(bool debug, const std::string& msg) {
    if (!debug) return;
    std::lock_guard<std::mutex> lk(g_log_mtx);
    std::ofstream log_file("streamer_debug.log", std::ios::app);
    
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    log_file << std::put_time(std::localtime(&now), "[%H:%M:%S] ") << msg << "\n";
}

// --- INTERNAL STATE ---
struct StreamState {
    lt::torrent_handle h;
    std::string file_path;
    std::int64_t file_size;
    std::int64_t file_offset;
    int piece_length;
    int num_pieces;
    int first_piece; 
    int last_piece;  
    std::mutex mtx;
    std::condition_variable cv;
    bool shutting_down = false;
    
    std::map<int, int> piece_refs; 
    std::atomic<int> current_request_id{0}; 
};

// --- MULTI-THREAD SAFE WINDOW MANAGER ---
struct WindowManager {
    StreamState& state;
    std::set<int> active_window;

    WindowManager(StreamState& s) : state(s) {}
    
    ~WindowManager() {
        std::lock_guard<std::mutex> lk(state.mtx);
        for (int p : active_window) {
            if (state.shutting_down) continue;
            state.piece_refs[p]--;
            if (state.piece_refs[p] <= 0) {
                state.piece_refs.erase(p);
                if (state.h.is_valid() && p <= state.last_piece && !state.h.have_piece(lt::piece_index_t(p))) {
                    state.h.piece_priority(lt::piece_index_t(p), lt::dont_download);
                    state.h.reset_piece_deadline(lt::piece_index_t(p));
                }
            }
        }
    }

    void update(int start_p, int end_p) {
        std::set<int> new_window;
        for(int p = start_p; p <= end_p; ++p) {
            if (p <= state.last_piece) new_window.insert(p);
        }

        std::lock_guard<std::mutex> lk(state.mtx);

        for(int p : new_window) {
            if (active_window.find(p) == active_window.end()) {
                state.piece_refs[p]++;
            }
        }

        for(int p : active_window) {
            if (new_window.find(p) == new_window.end()) {
                state.piece_refs[p]--;
                if (state.piece_refs[p] <= 0) {
                    state.piece_refs.erase(p);
                    if (!state.h.have_piece(lt::piece_index_t(p))) {
                        state.h.piece_priority(lt::piece_index_t(p), lt::dont_download);
                        state.h.reset_piece_deadline(lt::piece_index_t(p));
                    }
                }
            }
        }
        
        active_window = new_window;

        for(int p : active_window) {
            if (!state.h.have_piece(lt::piece_index_t(p))) {
                
                // THE FIX: Sliding Priority. 
                // Piece 0 gets priority 7. Piece 1 gets 6. Fades down to a minimum of 1.
                int prio_val = std::max(1, 7 - (p - start_p));
                state.h.piece_priority(lt::piece_index_t(p), lt::download_priority_t(static_cast<uint8_t>(prio_val)));
                
                // Keep the aggressive deadlines for the immediate playback head
                if (p <= start_p + 3) {
                    state.h.set_piece_deadline(lt::piece_index_t(p), (p - start_p) * 200);
                } else {
                    state.h.set_piece_deadline(lt::piece_index_t(p), (p - start_p) * 1000); 
                }
            }
        }
    }
};

// --- HELPERS ---
bool file_exists(const std::string& name) {
    struct stat buffer;   
    return (stat(name.c_str(), &buffer) == 0); 
}

std::string get_info_hash_string(const lt::torrent_info& ti) {
    std::stringstream ss;
    ss << ti.info_hash();
    return ss.str();
}

// --- ALERT LOOP ---
void alert_loop(lt::session& ses, StreamState* state, const std::string& resume_path, bool debug_mode) {
    write_debug_log(debug_mode, "[SYST] Alert Loop Thread Started");
    while (state && !state->shutting_down) {
        ses.wait_for_alert(lt::milliseconds(200));
        std::vector<lt::alert*> alerts;
        ses.pop_alerts(&alerts);

        for (lt::alert* a : alerts) {
            if (debug_mode && 
                a->type() != lt::piece_finished_alert::alert_type && 
                a->type() != lt::state_update_alert::alert_type &&
                a->type() != lt::block_downloading_alert::alert_type &&
                a->type() != lt::block_finished_alert::alert_type) {
                 write_debug_log(debug_mode, std::format("[ALRT] {}", a->message()));
            }

            if (lt::alert_cast<lt::piece_finished_alert>(a)) {
                state->cv.notify_all(); 
            }
            else if (auto* rd = lt::alert_cast<lt::save_resume_data_alert>(a)) {
                std::vector<char> buffer = lt::write_resume_data_buf(rd->params);
                std::ofstream of(resume_path, std::ios_base::binary);
                of.write(buffer.data(), buffer.size());
            }
        }
    }
}

// --- STREAMING LOGIC ---
void stream_file(lt::session& ses, AppConfig& config, lt::torrent_handle& h, 
                 std::shared_ptr<const lt::torrent_info> ti, int choice, const std::string& resume_path) {
    
    extern std::atomic<bool> interrupted; 
    interrupted = false; 

    lt::file_storage const& files = ti->files();
    StreamState state;
    state.h = h;
    state.file_path = config.save_dir + "/" + files.file_path(choice);
    state.file_size = files.file_size(choice);
    state.file_offset = files.file_offset(choice);
    state.piece_length = ti->piece_length();
    state.num_pieces = ti->num_pieces();
    state.first_piece = state.file_offset / state.piece_length;
    state.last_piece = (state.file_offset + state.file_size - 1) / state.piece_length;

    std::string hls_playlist = "";
    std::string selected_path = files.file_path(choice);
    bool debug = config.debug_mode;
    
    write_debug_log(debug, std::format("[INIT] Selected File: {} ({} bytes)", selected_path, state.file_size));
    
    std::vector<lt::download_priority_t> priorities(files.num_files(), lt::default_priority);
    h.prioritize_files(priorities);
    h.unset_flags(lt::torrent_flags::sequential_download);
    
    for (int p = 0; p < state.num_pieces; ++p) {
        h.piece_priority(lt::piece_index_t(p), lt::dont_download);
    }
    
    if (selected_path.length() >= 5 && selected_path.substr(selected_path.length() - 5) == ".m2ts") {
        std::string clpi_path = selected_path;
        size_t pos = clpi_path.find("STREAM");
        if (pos != std::string::npos) clpi_path.replace(pos, 6, "CLIPINF");
        pos = clpi_path.find(".m2ts");
        if (pos != std::string::npos) clpi_path.replace(pos, 5, ".clpi");

        int clpi_idx = -1;
        for (int i = 0; i < files.num_files(); ++i) {
            if (files.file_path(i) == clpi_path) { clpi_idx = i; break; }
        }

        if (clpi_idx != -1) {
            priorities[clpi_idx] = lt::top_priority;
            h.prioritize_files(priorities);
            std::vector<std::int64_t> fp;
            while(true) {
                h.file_progress(fp);
                if (fp.size() > clpi_idx && fp[clpi_idx] >= files.file_size(clpi_idx)) break;
                if (interrupted) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            std::string full_clpi = config.save_dir + "/" + files.file_path(clpi_idx);
            auto m_idx = parse_clpi_file(full_clpi);
            if (!m_idx.empty()) hls_playlist = generate_hls(m_idx, state.file_size, config.port);
        }
    }

    h.piece_priority(lt::piece_index_t(state.first_piece), lt::top_priority);
    h.piece_priority(lt::piece_index_t(state.last_piece), lt::top_priority);

    std::thread alert_thread(alert_loop, std::ref(ses), &state, resume_path, debug);

    while (!file_exists(state.file_path)) {
        if (interrupted) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    httplib::Server svr;
    svr.new_task_queue = [] { return new httplib::ThreadPool(64); };
    svr.set_read_timeout(3600, 0);
    svr.set_write_timeout(3600, 0);
    svr.set_keep_alive_max_count(100);
    svr.set_keep_alive_timeout(3600);
    
    if (debug) {
        svr.set_logger([debug](const httplib::Request& req, const httplib::Response& res) {
            std::string range = req.has_header("Range") ? req.get_header_value("Range") : "None";
            write_debug_log(debug, std::format("[HTTP] {} {} | Range: {} | HTTP Status: {}", req.method, req.path, range, res.status));
        });
    }

    svr.Get("/playlist.m3u8", [&hls_playlist](const httplib::Request&, httplib::Response& res) {
        if (!hls_playlist.empty()) res.set_content(hls_playlist, "application/vnd.apple.mpegurl");
        else res.status = 404;
    });

    svr.Get("/stream", [&state, debug](const httplib::Request& req, httplib::Response& res) {
        
        int my_id = ++state.current_request_id;
        write_debug_log(debug, std::format("[STRM] New connection established. Session ID: {}", my_id));

        std::string ext = state.file_path.substr(state.file_path.find_last_of('.') + 1);
        std::string mime_type = "video/mp4";
        if (ext == "mkv") mime_type = "video/x-matroska";
        else if (ext == "avi") mime_type = "video/x-msvideo";
        else if (ext == "m2ts" || ext == "ts") mime_type = "video/mp2t";

        res.set_header("Accept-Ranges", "bytes");

        auto wm = std::make_shared<WindowManager>(state);

        res.set_content_provider(state.file_size, mime_type,
            [&state, wm, my_id, debug](size_t offset, size_t length, httplib::DataSink& sink) {
                
                std::int64_t bytes_left = length;
                std::int64_t current_byte = offset; 
                std::ifstream file;

                while (bytes_left > 0) {
                    if (state.shutting_down || my_id <= state.current_request_id.load() - 6) {
                        return false;
                    }

                    int current_piece = (state.file_offset + current_byte) / state.piece_length;
                    int window_size = 20;
                    
                    wm->update(current_piece, current_piece + window_size); 

                    while (!state.h.have_piece(lt::piece_index_t(current_piece))) {
                        if (state.shutting_down || my_id <= state.current_request_id.load() - 6) return false;
                        
                        if (!sink.is_writable()) return false;

                        std::unique_lock<std::mutex> lk(state.mtx);
                        state.cv.wait_for(lk, std::chrono::milliseconds(200));
                    }

                    if (state.shutting_down) return false;
                    
                    // FIXED OVERFLOW BUG HERE: Cast `current_piece + 1` to 64-bit int BEFORE multiplication
                    std::int64_t piece_end_byte = std::min(
                        (static_cast<std::int64_t>(current_piece + 1) * state.piece_length) - state.file_offset - 1,
                        state.file_size - 1
                    );

                    std::int64_t chunk_size = std::min({
                        static_cast<std::int64_t>(256 * 1024), 
                        static_cast<std::int64_t>(bytes_left), 
                        piece_end_byte - current_byte + 1
                    });

                    if (!file.is_open()) file.open(state.file_path, std::ios::binary);
                    else file.clear(); 

                    file.seekg(current_byte);
                    std::vector<char> buffer(chunk_size);
                    file.read(buffer.data(), chunk_size);
                    
                    std::streamsize bytes_read = file.gcount();

                    if (bytes_read == 0) {
                        file.close(); 
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        continue; 
                    }

                    if (!sink.write(buffer.data(), bytes_read)) return false;

                    current_byte += bytes_read;
                    bytes_left -= bytes_read;
                }
                return true; 
            }
        );
    });

    std::thread server_thread([&]() { svr.listen("localhost", config.port); });
    
    std::string launch_url = hls_playlist.empty() ? 
                             std::format("http://localhost:{}/stream", config.port) : 
                             std::format("http://localhost:{}/playlist.m3u8", config.port);
                             
    std::cout << "\n[*] Launching " << (hls_playlist.empty() ? "raw stream" : "HLS timeline stream") << "...\n";
    launch_player(config, launch_url);
    std::cout << "\n[!] STREAM ACTIVE: Press Ctrl+C to STOP and RETURN TO MENU.\n\n";

    while (!interrupted) {
        lt::torrent_status st = h.status();
        
        std::vector<std::int64_t> fp;
        h.file_progress(fp); // FIXED PROGRESS BUG HERE: Removed piece_granularity flag
        
        double actual_progress = 0.0;
        if (!fp.empty() && choice < fp.size() && state.file_size > 0) {
            actual_progress = (static_cast<double>(fp[choice]) / static_cast<double>(state.file_size)) * 100.0;
        }

        // THE FIX: Standard forward iterator. Now that old pieces are killed quickly,
        // this cleanly prints the new pieces from Lowest (Current Position) to Highest (Buffer End)
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

        std::cout << "\r\033[K[>] DL: " << (st.download_rate / 1000) << " kB/s | Progress: " 
                  << std::fixed << std::setprecision(2) << actual_progress << "% | Peers: " 
                  << st.num_peers << " | Tracked: " << active_pieces << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::cout << "\n\n[*] Stopping player and returning to file menu...\n";
    stop_player();
    
    state.shutting_down = true;
    state.cv.notify_all();
    
    h.save_resume_data();
    
    svr.stop();
    server_thread.join();
    alert_thread.join();
    
    for (int p = 0; p < state.num_pieces; ++p) {
        h.piece_priority(lt::piece_index_t(p), lt::dont_download);
    }
    h.clear_piece_deadlines();

    interrupted = false; 
    std::cin.clear();
}

void handle_torrent(lt::session& ses, AppConfig& config, std::string source) {
    extern std::atomic<bool> interrupted;
    interrupted = false;
    std::cin.clear();

    lt::settings_pack pack;
    pack.set_str(lt::settings_pack::listen_interfaces, "0.0.0.0:6881");
    pack.set_int(lt::settings_pack::alert_mask, 
        lt::alert_category::error | 
        lt::alert_category::status | 
        lt::alert_category::storage | 
        lt::alert_category::file_progress);
        
    ses.apply_settings(pack);

    lt::add_torrent_params atp;
    std::string hash_str;

    if (file_exists(source) && source.find(".torrent") != std::string::npos) {
        atp.ti = std::make_shared<lt::torrent_info>(source);
        hash_str = get_info_hash_string(*atp.ti);
    } else {
        atp = lt::parse_magnet_uri(source);
        std::stringstream ss;
        ss << atp.info_hashes.get_best();
        hash_str = ss.str();
    }

    atp.save_path = config.save_dir;
    std::string torrent_file_path = config.save_dir + "/" + hash_str + ".torrent";
    std::string resume_file_path = config.save_dir + "/" + hash_str + ".fastresume";

    if (!atp.ti && file_exists(torrent_file_path)) {
        atp.ti = std::make_shared<lt::torrent_info>(torrent_file_path);
    }

    if (file_exists(resume_file_path)) {
        std::ifstream ifs(resume_file_path, std::ios_base::binary);
        ifs.unsetf(std::ios_base::skipws);
        std::vector<char> buf{std::istream_iterator<char>(ifs), std::istream_iterator<char>()};
        lt::error_code ec;
        lt::add_torrent_params resume_params = lt::read_resume_data(buf, ec);
        if (!ec) {
            auto ti_backup = atp.ti; 
            atp = resume_params;
            atp.ti = ti_backup;
            atp.save_path = config.save_dir;
        }
    }

    lt::torrent_handle h = ses.add_torrent(atp);

    if (!h.status().has_metadata) {
        std::cout << "\n[*] Waiting for Metadata...\n";
        while (!h.status().has_metadata) {
            if (interrupted) { ses.remove_torrent(h); interrupted = false; return; }
            lt::torrent_status st = h.status();
            std::cout << "\r[>] DHT/LSD Peers: " << st.num_peers << " | Searching...   " << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        
        std::shared_ptr<const lt::torrent_info> ti_new = h.torrent_file();
        std::ofstream f(torrent_file_path, std::ios_base::binary);
        std::vector<char> buf;
        lt::bencode(std::back_inserter(buf), lt::create_torrent(*ti_new).generate());
        f.write(buf.data(), buf.size());
    }
    
    std::shared_ptr<const lt::torrent_info> ti = h.torrent_file();

    while (true) {
        interrupted = false; 
        std::cin.clear();
        std::cout << "\n\n============================================================\n";
        std::cout << "                 AVAILABLE FILES\n";
        std::cout << "============================================================\n";
        lt::file_storage const& files = ti->files();
        for (int i = 0; i < files.num_files(); ++i) {
            std::cout << " [" << i << "] " << files.file_path(i) 
                      << " (" << files.file_size(i) / (1024 * 1024) << " MB)\n";
        }

        std::string input;
        std::cout << "\n[?] Enter file number, 'b' to go back, 'q' to quit: ";
        
        std::getline(std::cin, input);

        if (interrupted) {
            interrupted = false;
            std::cin.clear();
            input = "b"; 
        }

        auto start = input.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue; 
        input = input.substr(start, input.find_last_not_of(" \t\r\n") - start + 1);

        if (input == "b" || input == "B") {
            h.save_resume_data();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            ses.remove_torrent(h);
            return;
        }
        if (input == "q" || input == "Q") {
            h.save_resume_data();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            exit(0);
        }

        int choice = -1;
        try { choice = std::stoi(input); } catch(...) {}

        if (choice < 0 || choice >= files.num_files()) {
            std::cerr << "[-] Invalid selection. Try again.\n";
            continue;
        }

        stream_file(ses, config, h, ti, choice, resume_file_path);
    }
}


