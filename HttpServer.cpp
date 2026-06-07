#include "HttpServer.h"
#include "WindowManager.h"
#include "Utils.h"
#include "DirectLinkEngine.h" 
#include "ProcessManager.h" 
#include <thread>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

extern std::atomic<bool> interrupted;
extern std::unordered_map<std::string, DirectStreamHandle> active_direct_streams;

void run_http_server(httplib::Server& svr, TorrentManager& manager, const std::string& hls_playlist, AppConfig& config) {
    bool debug = config.debug_mode;
    
    svr.new_task_queue = [] { return new httplib::ThreadPool(64); };
    
    if (debug) {
        svr.set_logger([debug](const httplib::Request& req, const httplib::Response& res) {
            std::string range = req.has_header("Range") ? req.get_header_value("Range") : "None";
            write_debug_log(debug, "[HTTP] {} {} | Range: {} | HTTP Status: {}", req.method, req.path, range, res.status);
        });
    }

    // --- 1. MOUNT THE WEB UI DIRECTORY ---
    svr.set_mount_point("/", "./public");
    
    // --- 2. BUILD THE REST API (STATUS) ---
    svr.Get("/api/status", [&manager, &config](const httplib::Request&, httplib::Response& res) {
        json response = {
            {"torrents", json::array()},
            {"direct_streams", json::array()}
        };

        // Grab active torrents
        {
            std::shared_lock<std::shared_mutex> lock(manager.registry_mtx);
            for (const auto& [hash, state] : manager.active_streams) {
                response["torrents"].push_back({
                    {"id", hash},
                    {"name", state->file_path.substr(state->file_path.find_last_of('/') + 1)},
                    {"url", "http://localhost:" + std::to_string(config.port) + "/stream/" + hash}
                });
            }
        }

        // Grab active web streams
        for (const auto& [id, handle] : active_direct_streams) {
            response["direct_streams"].push_back({
                {"id", id},
                {"url", handle.stream_id} 
            });
        }

        res.set_content(response.dump(), "application/json");
    });

    // --- 3. BUILD THE REST API (STOP STREAM) ---
    svr.Post("/api/stop", [&manager, debug](const httplib::Request& req, httplib::Response& res) {
        json req_body;
        try {
            req_body = json::parse(req.body);
        } catch (...) {
            res.status = 400;
            return;
        }

        std::string target = req_body.value("id", "");
        bool found = false;

        // 1. Search Direct Web Streams
        auto it_dir = std::find_if(active_direct_streams.begin(), active_direct_streams.end(),
                               [&target](const auto& pair) { return pair.first.find(target) != std::string::npos; });

        if (it_dir != active_direct_streams.end()) {
            it_dir->second.cancel_token->store(true);
            stop_player_by_pid(it_dir->second.player_pid);
            active_direct_streams.erase(it_dir);
            found = true;
        } 
        // 2. Search BitTorrent Streams
        else {
            std::unique_lock<std::shared_mutex> lock(manager.registry_mtx);
            auto it_tor = std::find_if(manager.active_streams.begin(), manager.active_streams.end(),
                                   [&target](const auto& pair) { return pair.first.find(target) != std::string::npos; });

            if (it_tor != manager.active_streams.end()) {
                auto state = it_tor->second;
                state->shutting_down.store(true);
                state->cv.notify_all(); 
                stop_player_by_pid(state->player_pid); 
                manager.ses.remove_torrent(state->h); 
                manager.active_streams.erase(it_tor);
                found = true;
            }
        }

        if (found) {
            write_debug_log(debug, "[HTTP] API successfully killed stream: {}", target);
            res.set_content(R"({"status":"success"})", "application/json");
        } else {
            res.status = 404;
            res.set_content(R"({"status":"error", "message":"Stream not found"})", "application/json");
        }
    });

    svr.Get("/playlist.m3u8", [&hls_playlist](const httplib::Request&, httplib::Response& res) {
        if (!hls_playlist.empty()) res.set_content(hls_playlist, "application/vnd.apple.mpegurl");
        else res.status = 404;
    });

    svr.Get(R"(/abort/([a-fA-F0-9_]+))", [&manager, debug](const httplib::Request& req, httplib::Response& res) {
        std::string hash = req.matches[1];
        if (auto state_ptr = manager.get_stream(hash)) {
            state_ptr->current_request_id++; 
            write_debug_log(debug, "[STRM] ABORT endpoint triggered for hash: {}", hash);
            res.set_content("Aborted", "text/plain");
        } else {
            res.status = 404;
        }
    });

    svr.Get(R"(/stream/([a-fA-F0-9_]+))", [&manager, debug](const httplib::Request& req, httplib::Response& res) {
        std::string hash = req.matches[1];
        auto state_ptr = manager.get_stream(hash); 
        
        if (!state_ptr) {
            write_debug_log(debug, "[HTTP] 404 - Torrent Hash Not Found: {}", hash);
            res.status = 404;
            return;
        }

        static std::atomic<int> session_counter{0};
        int my_session_id = ++session_counter;
        int my_epoch = state_ptr->current_request_id.load(); 

        write_debug_log(debug, "[STRM] New connection. Session ID: {} for Hash: {}", my_session_id, hash);

        std::string ext = state_ptr->file_path.substr(state_ptr->file_path.find_last_of('.') + 1);
        std::string mime_type = "video/mp4";
        if (ext == "mkv") mime_type = "video/x-matroska";
        else if (ext == "avi") mime_type = "video/x-msvideo";
        else if (ext == "m2ts" || ext == "ts") mime_type = "video/mp2t";
        else if (ext == "iso") mime_type = "application/x-iso9660-image";

        res.set_header("Connection", "close");
        res.set_header("Accept-Ranges", "bytes");

        auto wm = std::make_shared<WindowManager>(*state_ptr);

        res.set_content_provider(state_ptr->file_size, mime_type,
            [state_ptr, wm, my_session_id, my_epoch, debug](size_t offset, size_t length, httplib::DataSink& sink) {
                
                StreamState& state = *state_ptr; 
                std::int64_t bytes_left = length;
                std::int64_t current_byte = offset; 
                std::ifstream file;
                int empty_reads = 0; 

                while (bytes_left > 0) {
                    if (state.shutting_down.load() || interrupted.load()) return false; 

                    int current_piece = (state.file_offset + current_byte) / state.piece_length;
                    state.latest_piece_requested = current_piece; 
                    
                    std::int64_t target_buffer_bytes = 15 * 1024 * 1024; 
                    int dynamic_window = static_cast<int>(target_buffer_bytes / state.piece_length);
                    int window_size = std::clamp(dynamic_window, 4, 12);
                                        
                    wm->update(current_piece, current_piece + window_size); 

                    while (!state.h.have_piece(lt::piece_index_t(current_piece))) {
                        if (state.shutting_down.load() || interrupted.load()) return false;
                        
                        if (!sink.is_writable()) {
                            write_debug_log(debug, "[STRM] Socket dead. Session ID: {}", my_session_id); 
                            return false;
                        }

                        if (my_epoch < state.current_request_id.load()) {
                            write_debug_log(debug, "[STRM] Active Kill Detected! Session ID: {}", my_session_id); 
                            return false; 
                        }

                        std::unique_lock<std::mutex> lk(state.mtx);
                        state.cv.wait_for(lk, std::chrono::milliseconds(200));
                    }

                    if (state.shutting_down.load() || interrupted.load()) return false;
                    
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
                        empty_reads++; 
                        if (empty_reads > 100) return false; 
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        continue; 
                    }
                    empty_reads = 0; 

                    if (!sink.write(buffer.data(), bytes_read)) return false;

                    current_byte += bytes_read;
                    bytes_left -= bytes_read;
                }
                return true; 
            }
        );
    });
}
