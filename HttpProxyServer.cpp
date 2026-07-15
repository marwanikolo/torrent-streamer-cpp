#include "HttpProxyServer.h"
#include "Utils.h"
#include <print>
#include <format>
#include <algorithm>
#include <thread>
#include <fstream>
#include <filesystem>

HttpProxyServer::HttpProxyServer(HttpCacheManager& cache, HttpDownloader& downloader, const std::string& video_url, int port, const std::string& stream_id, bool debug, const httplib::Headers& headers, bool is_hls, const std::string& hls_playlist, const std::vector<std::string>& hls_chunks, const std::string& hls_save_dir)
    : cache_(cache), downloader_(downloader), video_url_(video_url), port_(port), stream_id_(stream_id), debug_(debug), extra_headers_(headers), is_hls_(is_hls), hls_playlist_text_(hls_playlist), hls_chunk_urls_(hls_chunks), hls_save_dir_(hls_save_dir) {
    
    size_t protocol_pos = video_url_.find("://");
    size_t host_start = (protocol_pos != std::string::npos) ? protocol_pos + 3 : 0;
    size_t path_start = video_url_.find('/', host_start);

    if (path_start == std::string::npos) {
        host_ = video_url_;
        path_ = "/";
    } else {
        host_ = video_url_.substr(0, path_start);
        path_ = video_url_.substr(path_start);
    }
    setup_routes();
}

HttpProxyServer::~HttpProxyServer() {
    stop();
}

void HttpProxyServer::start() {
    svr_.new_task_queue = [] { return new httplib::ThreadPool(64); };
    
    server_thread_ = std::jthread([this](std::stop_token stoken) {
        svr_.listen("0.0.0.0", port_);
    });
}

void HttpProxyServer::stop() {
    if (is_shutting_down_.load()) return;
    is_shutting_down_ = true;
    current_request_id_++; 
    
    svr_.stop();
    
    std::jthread unblocker([this]() {
        httplib::Client cli("localhost", port_);
        cli.set_connection_timeout(0, 100000); 
        cli.Get("/"); 
    });
}

void HttpProxyServer::setup_routes() {
    if (debug_) {
        svr_.set_logger([this](const httplib::Request& req, const httplib::Response& res) {
            std::string range = req.has_header("Range") ? req.get_header_value("Range") : "None";
            write_debug_log(debug_, "[PROX] {} {} | Range: {} | Status: {}", req.method, req.path, range, res.status);
        });
    }

    if (is_hls_) {
        std::string playlist_route = "/playlist.m3u8";
        svr_.Get(playlist_route, [this](const httplib::Request&, httplib::Response& res) {
            write_debug_log(debug_, "[PROX] Serving modified HLS playlist to player.");
            res.set_content(hls_playlist_text_, "application/vnd.apple.mpegurl");
        });

        // Regex now optionally matches the file extension (e.g., .ts or .m4s)
        std::string chunk_route = R"(/chunk/(\d+)(?:\.([a-zA-Z0-9]+))?)";
        svr_.Get(chunk_route, [this](const httplib::Request& req, httplib::Response& res) {
            
            // Bypass cpp-httplib's internal auto-slicer
            const_cast<httplib::Request&>(req).ranges.clear();

            int chunk_index = 0;
            std::string ext = "ts"; // Default fallback
            
            try {
                chunk_index = std::stoi(req.matches[1]);
                // Safely extract the extension if it exists in the regex match
                if (req.matches.size() > 2 && req.matches[2].matched) {
                    ext = req.matches[2].str();
                }
            } catch (...) {
                res.status = 404;
                return;
            }
            
            if (chunk_index < 0 || chunk_index >= hls_chunk_urls_.size()) {
                res.status = 404;
                return;
            }

            // Set MIME type dynamically based on the extension
            std::string mime_type = (ext == "m4s" || ext == "mp4") ? "video/mp4" : "video/MP2T";
            
            // Use the captured extension for the saved file path
            std::string chunk_file_path = std::format("{}/chunk_{}.{}", hls_save_dir_, chunk_index, ext);

            std::error_code ec;
            if (std::filesystem::exists(chunk_file_path, ec)) {
                std::ifstream ifs(chunk_file_path, std::ios::binary | std::ios::ate);
                if (ifs) {
                    std::streamsize size = ifs.tellg();
                    ifs.seekg(0, std::ios::beg);
                    std::string buffer(size, '\0');
                    
                    if (ifs.read(buffer.data(), size)) {
                        write_debug_log(debug_, "[PROX] HLS Cache HIT: Serving chunk {} from disk.", chunk_index);
                        
                        // Manually spoof the 206 response for the player
                        if (req.has_header("Range")) {
                            res.status = 206; 
                            std::string range_val = req.get_header_value("Range");
                            if (range_val.starts_with("bytes=")) {
                                std::string range_range = range_val.substr(6);
                                res.set_header("Content-Range", std::format("bytes {}/*", range_range));
                            }
                        }
                        
                        // Use dynamic MIME type on Cache Hit
                        res.set_content(buffer, mime_type.c_str());
                        return; 
                    }
                }
            }

            std::string target_chunk_url = hls_chunk_urls_[chunk_index];
            std::string c_host, c_path;
            size_t protocol_pos = target_chunk_url.find("://");
            size_t host_start = (protocol_pos != std::string::npos) ? protocol_pos + 3 : 0;
            size_t path_start = target_chunk_url.find('/', host_start);
            c_host = target_chunk_url.substr(0, path_start);
            c_path = target_chunk_url.substr(path_start);

            // Forward the Range header to the CDN
            httplib::Headers fetch_headers = extra_headers_;
            if (req.has_header("Range")) {
                fetch_headers.emplace("Range", req.get_header_value("Range"));
            }

            httplib::Client cli(c_host);
            cli.set_follow_location(true);
            cli.enable_server_certificate_verification(false); 
            
            auto web_res = cli.Get(c_path.c_str(), fetch_headers);

            if (web_res && (web_res->status == 200 || web_res->status == 206)) {
                std::ofstream ofs(chunk_file_path, std::ios::binary);
                if (ofs) {
                    ofs.write(web_res->body.data(), web_res->body.size());
                }
                
                // Forward the partial content status and upstream content type
                res.status = web_res->status;
                if (web_res->has_header("Content-Range")) {
                    res.set_header("Content-Range", web_res->get_header_value("Content-Range"));
                }
                
                // Fallback to dynamic MIME type if CDN doesn't provide Content-Type
                std::string content_type = web_res->has_header("Content-Type") 
                                           ? web_res->get_header_value("Content-Type") 
                                           : mime_type;
                
                res.set_content(web_res->body, content_type.c_str());
            } else {
                write_debug_log(debug_, "[PROX] Failed to fetch CDN chunk {}. Status: {}", chunk_index, web_res ? web_res->status : 0);
                res.status = 404;
            }
        });
    }

    std::string abort_route = "/abort/" + stream_id_;
    svr_.Get(abort_route, [this](const httplib::Request&, httplib::Response& res) {
        current_request_id_++;
        write_debug_log(debug_, "[PROX] ABORT endpoint triggered by Lua hook. Severing old streams.");
        res.set_content("Aborted", "text/plain");
    });

    std::string stream_route = "/stream/" + stream_id_;
    svr_.Get(stream_route, [this](const httplib::Request& req, httplib::Response& res) {
        int my_id = ++current_request_id_;
        std::int64_t file_size = cache_.get_file_size();

        res.set_header("Connection", "close");
        res.set_header("Accept-Ranges", "bytes");

        res.set_content_provider(file_size, "video/mp4",
            [this, my_id, file_size](size_t offset, size_t total_length, httplib::DataSink& sink) {
                
                std::int64_t current_byte = offset;
                std::int64_t bytes_left = total_length;
                
                size_t start_chunk = current_byte / cache_.get_chunk_size();
                if (current_byte < file_size - (5 * 1024 * 1024)) downloader_.update_playhead(start_chunk);

                httplib::Client cli(host_);
                cli.set_keep_alive(true);
                cli.set_follow_location(true);
                cli.enable_server_certificate_verification(false);

                while (bytes_left > 0) {
                    if (is_shutting_down_ || my_id < current_request_id_) return false;

                    size_t chunk_idx = current_byte / cache_.get_chunk_size();
                    std::int64_t chunk_end = std::min<std::int64_t>(((chunk_idx + 1) * cache_.get_chunk_size()) - 1, file_size - 1);

                    if (cache_.has_chunk(chunk_idx)) {
                        std::int64_t read_len = std::min<std::int64_t>(bytes_left, chunk_end - current_byte + 1);
                        std::vector<char> buf(read_len);
                        
                        size_t bytes_read = cache_.read_data(current_byte, buf.data(), read_len);
                        if (bytes_read > 0) {
                            sink.write(buf.data(), bytes_read);
                            current_byte += bytes_read;
                            bytes_left -= bytes_read;
                            continue;
                        }
                    }

                    // --- PRIORITY HANDOFF: The proxy needs data instantly. Claim the network. ---
                    cache_.request_network_priority();

                    // Sleep for 150ms. This ensures the background downloader's callback trips, 
                    // returns false, and closes its TCP socket so we don't trigger the CDN's concurrent connection limit.
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));

                    // Now that the background downloader has yielded, lock the chunk.
                    if (!cache_.try_lock_chunk(chunk_idx)) {
                        cache_.release_network_priority();
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        continue;
                    }

                    std::int64_t req_end = std::min<std::int64_t>(current_byte + bytes_left - 1, chunk_end);
                    
                    httplib::Headers req_headers = extra_headers_;
                    req_headers.emplace("Range", std::format("bytes={}-{}", current_byte, req_end));
                    
                    bool abort_proxy = false;
                    
                    auto web_res = cli.Get(path_.c_str(), req_headers, 
                        [&](const httplib::Response& response) {
                            if (response.status != 200 && response.status != 206) {
                                write_debug_log(debug_, "[PROX] CRITICAL: CDN returned HTTP {}. Aborting stream to prevent cache poisoning.", response.status);
                                std::println(stderr, "[-] Proxy Error: CDN returned HTTP {}", response.status);
                                
                                std::this_thread::sleep_for(std::chrono::seconds(3));
                                abort_proxy = true;
                                return false; 
                            }
                            return true;
                        },
                        [&](const char *data, size_t data_length) {
                            if (is_shutting_down_ || my_id < current_request_id_) {
                                abort_proxy = true;
                                return false; 
                            }
                            // The proxy streams directly to the player, bypassing the read-cache deadlock entirely
                            sink.write(data, data_length);
                            cache_.write_data(current_byte, data, data_length);
                            current_byte += data_length;
                            bytes_left -= data_length;
                            return true;
                        }
                    );

                    if (current_byte > chunk_end) {
                        cache_.set_chunk(chunk_idx);
                    }
                    
                    // --- RELEASE: Unlock the chunk and yield the network back to the background worker ---
                    cache_.unlock_chunk(chunk_idx);
                    cache_.release_network_priority(); 
                    
                    if (abort_proxy || !web_res) return false;
                }
                return true; 
            }
        );
    });
}
