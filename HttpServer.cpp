#include "HttpServer.h"
#include "WindowManager.h"
#include "Utils.h"
#include <thread>
#include <chrono>
#include <fstream>
#include <algorithm>
// (Removed <format> as we no longer need the wrapper)

extern std::atomic<bool> interrupted;

void run_http_server(httplib::Server& svr, StreamState& state, const std::string& hls_playlist, AppConfig& config) {
    bool debug = config.debug_mode;
    
    svr.new_task_queue = [] { return new httplib::ThreadPool(64); };
    
    if (debug) {
        svr.set_logger([debug](const httplib::Request& req, const httplib::Response& res) {
            std::string range = req.has_header("Range") ? req.get_header_value("Range") : "None";
            // NEW C++23: Pass arguments directly!
            write_debug_log(debug, "[HTTP] {} {} | Range: {} | HTTP Status: {}", req.method, req.path, range, res.status);
        });
    }

    svr.Get("/playlist.m3u8", [&hls_playlist](const httplib::Request&, httplib::Response& res) {
        if (!hls_playlist.empty()) res.set_content(hls_playlist, "application/vnd.apple.mpegurl");
        else res.status = 404;
    });

    svr.Get("/abort", [&state, debug](const httplib::Request&, httplib::Response& res) {
        state.current_request_id++;
        write_debug_log(debug, "[STRM] ABORT endpoint triggered by Lua hook.");
        res.set_content("Aborted", "text/plain");
    });

    svr.Get("/stream", [&state, debug](const httplib::Request& req, httplib::Response& res) {
        
        int my_id = ++state.current_request_id;
        write_debug_log(debug, "[STRM] New connection established. Session ID: {}", my_id); // NEW

        std::string ext = state.file_path.substr(state.file_path.find_last_of('.') + 1);
        std::string mime_type = "video/mp4";
        if (ext == "mkv") mime_type = "video/x-matroska";
        else if (ext == "avi") mime_type = "video/x-msvideo";
        else if (ext == "m2ts" || ext == "ts") mime_type = "video/mp2t";

        res.set_header("Connection", "close");
        res.set_header("Accept-Ranges", "bytes");

        auto wm = std::make_shared<WindowManager>(state);

        res.set_content_provider(state.file_size, mime_type,
            [&state, wm, my_id, debug](size_t offset, size_t length, httplib::DataSink& sink) {
                
                int start_piece = (state.file_offset + offset) / state.piece_length;

                if (start_piece > state.first_piece + 5) {
                    std::vector<int> availability;
                    state.h.piece_availability(availability);
                    
                    if (!availability.empty() && start_piece < availability.size() && availability[start_piece] == 0) {
                        write_debug_log(debug, "[STRM] FATAL: Seek piece {} has 0 availability. Dropping connection to prevent freeze.", start_piece); // NEW
                        return false; 
                    }
                }

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
                        if (state.shutting_down.load() || interrupted.load() || my_id <= state.current_request_id.load() - 6) return false;
                        
                        if (!sink.is_writable()) {
                            write_debug_log(debug, "[STRM] Socket dead (sink not writable). Aborting Session ID: {}", my_id); // NEW
                            return false;
                        }

                        if (my_id < state.current_request_id.load()) {
                            write_debug_log(debug, "[STRM] Active Kill Detected! Instantly aborting obsolete Session ID: {}", my_id); // NEW
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
