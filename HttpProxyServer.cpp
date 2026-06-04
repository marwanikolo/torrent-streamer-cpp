#include "HttpProxyServer.h"
#include "Utils.h"
#include <print>
#include <format>
#include <algorithm>

HttpProxyServer::HttpProxyServer(HttpCacheManager& cache, HttpDownloader& downloader, const std::string& video_url, int port, bool debug)
    : cache_(cache), downloader_(downloader), video_url_(video_url), port_(port), debug_(debug) {
    
    // Parse URL into host and path for the cpp-httplib client
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
    
    server_thread_ = std::thread([this]() {
        std::println("[*] Proxy Server listening on port {}...", port_);
        svr_.listen("localhost", port_);
    });
}

void HttpProxyServer::stop() {
    if (is_shutting_down_.load()) return;
    is_shutting_down_ = true;
    current_request_id_++; // Instantly sever all active proxy streams
    
    svr_.stop();
    
    // Dummy request to unblock the listen loop
    std::thread([this]() {
        httplib::Client cli("localhost", port_);
        cli.set_connection_timeout(0, 100000); 
        cli.Get("/"); 
    }).detach();

    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

void HttpProxyServer::setup_routes() {
    if (debug_) {
        svr_.set_logger([this](const httplib::Request& req, const httplib::Response& res) {
            std::string range = req.has_header("Range") ? req.get_header_value("Range") : "None";
            write_debug_log(debug_, "[PROXY] {} {} | Range: {} | Status: {}", req.method, req.path, range, res.status);
        });
    }

    // THE ACTIVE KILL HOOK
    svr_.Get("/abort", [this](const httplib::Request&, httplib::Response& res) {
        current_request_id_++;
        write_debug_log(debug_, "[PROXY] ABORT endpoint triggered by Lua hook. Severing old streams.");
        res.set_content("Aborted", "text/plain");
    });

    // THE STREAM ENGINE
    svr_.Get("/stream", [this](const httplib::Request& req, httplib::Response& res) {
        int my_id = ++current_request_id_;
        std::int64_t file_size = cache_.get_file_size();

        res.set_header("Connection", "close");
        res.set_header("Accept-Ranges", "bytes");

        // Let cpp-httplib parse the Range headers automatically!
        res.set_content_provider(file_size, "video/mp4",
            [this, my_id, file_size](size_t offset, size_t total_length, httplib::DataSink& sink) {
                
                std::int64_t current_byte = offset;
                std::int64_t bytes_left = total_length;
                
                // Steer the background downloader to the user's new seek position
                size_t start_chunk = current_byte / cache_.get_chunk_size();
                if (current_byte < file_size - (5 * 1024 * 1024)) {
                    downloader_.update_playhead(start_chunk);
                }

                // Reusable client for cache misses
                httplib::Client cli(host_);
                cli.set_keep_alive(true);
                cli.set_follow_location(true);

                while (bytes_left > 0) {
                    // Check for Active Kill or Application Shutdown
                    if (is_shutting_down_ || my_id < current_request_id_) return false;

                    size_t chunk_idx = current_byte / cache_.get_chunk_size();
                    std::int64_t chunk_end = std::min<std::int64_t>(
                        ((chunk_idx + 1) * cache_.get_chunk_size()) - 1, 
                        file_size - 1
                    );

                    // 1. LOCAL CACHE PATH
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

                    // 2. WEB PROXY PATH (Cache Miss)
                    // Request exactly what we need up to the chunk boundary
                    std::int64_t req_end = std::min<std::int64_t>(current_byte + bytes_left - 1, chunk_end);
                    
                    httplib::Headers headers = {
                        {"Range", std::format("bytes={}-{}", current_byte, req_end)}
                    };
                    
                    bool abort_proxy = false;
                    auto web_res = cli.Get(path_.c_str(), headers,
                        [&](const char *data, size_t data_length) {
                            if (is_shutting_down_ || my_id < current_request_id_) {
                                abort_proxy = true;
                                return false; // Instantly kills the HTTP socket
                            }
                            sink.write(data, data_length);
                            cache_.write_data(current_byte, data, data_length);
                            
                            current_byte += data_length;
                            bytes_left -= data_length;
                            return true;
                        }
                    );

                    if (abort_proxy) return false;

                    // If we successfully downloaded the full bounds of the chunk, flag it as complete!
                    if (current_byte > chunk_end) {
                        cache_.set_chunk(chunk_idx);
                        write_debug_log(debug_, "[PROXY] Chunk {} completely cached on the fly.", chunk_idx);
                    }
                }
                return true; 
            }
        );
    });
}
