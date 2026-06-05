#include "HttpServer.h"
#include "WindowManager.h"
#include "Utils.h"
#include <thread>
#include <chrono>
#include <fstream>
#include <algorithm>

extern std::atomic<bool> interrupted;

void run_http_server(httplib::Server& svr, StreamState& state, const std::string& hls_playlist, AppConfig& config) {
    bool debug = config.debug_mode;
    
    svr.new_task_queue = [] { return new httplib::ThreadPool(64); };
    
    if (debug) {
        svr.set_logger([debug](const httplib::Request& req, const httplib::Response& res) {
            std::string range = req.has_header("Range") ? req.get_header_value("Range") : "None";
            write_debug_log(debug, "[HTTP] {} {} | Range: {} | HTTP Status: {}", req.method, req.path, range, res.status);
        });
    }

    svr.Get("/playlist.m3u8", [&hls_playlist](const httplib::Request&, httplib::Response& res) {
        if (!hls_playlist.empty()) res.set_content(hls_playlist, "application/vnd.apple.mpegurl");
        else res.status = 404;
    });

    // THIS IS THE ONLY ENDPOINT THAT SHOULD INCREMENT THE KILL EPOCH
    svr.Get("/abort", [&state, debug](const httplib::Request&, httplib::Response& res) {
        state.current_request_id++; 
        write_debug_log(debug, "[STRM] ABORT endpoint triggered by Lua hook. Severing old streams.");
        res.set_content("Aborted", "text/plain");
    });

    svr.Get("/stream", [&state, debug](const httplib::Request& req, httplib::Response& res) {
        
        // FIX: Decouple the Session Counter from the Global Kill Epoch
        static std::atomic<int> session_counter{0};
        int my_session_id = ++session_counter;
        int my_epoch = state.current_request_id.load(); 

        write_debug_log(debug, "[STRM] New connection established. Session ID: {}", my_session_id);

        std::string ext = state.file_path.substr(state.file_path.find_last_of('.') + 1);
        std::string mime_type = "video/mp4";
        if (ext == "mkv") mime_type = "video/x-matroska";
        else if (ext == "avi") mime_type = "video/x-msvideo";
        else if (ext == "m2ts" || ext == "ts") mime_type = "video/mp2t";
        else if (ext == "iso") mime_type = "application/x-iso9660-image"; // <--- NEW: ISO Disk Image Mounting!

        res.set_header("Connection", "close");
        res.set_header("Accept-Ranges", "bytes");

        auto wm = std::make_shared<WindowManager>(state);

        res.set_content_provider(state.file_size, mime_type,
            [&state, wm, my_session_id, my_epoch, debug](size_t offset, size_t length, httplib::DataSink& sink) {
                
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
                            write_debug_log(debug, "[STRM] Socket dead (sink not writable). Aborting Session ID: {}", my_session_id); 
                            return false;
                        }

                        // FIX: Use the locked-in Epoch to check if the Lua hook fired
                        if (my_epoch < state.current_request_id.load()) {
                            write_debug_log(debug, "[STRM] Active Kill Detected! Instantly aborting obsolete Session ID: {}", my_session_id); 
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
