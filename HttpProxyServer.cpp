#include "HttpProxyServer.h"
#include "Utils.h"
#include <print>
#include <format>
#include <algorithm>
#include <thread>

HttpProxyServer::HttpProxyServer(HttpCacheManager& cache, HttpDownloader& downloader, const std::string& video_url, int port, const std::string& stream_id, bool debug, const httplib::Headers& headers)
    : cache_(cache), downloader_(downloader), video_url_(video_url), port_(port), stream_id_(stream_id), debug_(debug), extra_headers_(headers) {
    
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
    
    // --- UPGRADED: Assign to std::jthread ---
    server_thread_ = std::jthread([this](std::stop_token stoken) {
        svr_.listen("0.0.0.0", port_);
    });
}

void HttpProxyServer::stop() {
    if (is_shutting_down_.load()) return;
    is_shutting_down_ = true;
    current_request_id_++; 
    
    svr_.stop();
    
    // --- UPGRADED: Temporary jthread unblocker ---
    // The destructor of this jthread will safely block until the GET request finishes
    std::jthread unblocker([this]() {
        httplib::Client cli("localhost", port_);
        cli.set_connection_timeout(0, 100000); 
        cli.Get("/"); 
    });
    
    // We no longer need server_thread_.join() because jthread handles it!
}

void HttpProxyServer::setup_routes() {
    if (debug_) {
        svr_.set_logger([this](const httplib::Request& req, const httplib::Response& res) {
            std::string range = req.has_header("Range") ? req.get_header_value("Range") : "None";
            write_debug_log(debug_, "[PROX] {} {} | Range: {} | Status: {}", req.method, req.path, range, res.status);
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

                    std::int64_t req_end = std::min<std::int64_t>(current_byte + bytes_left - 1, chunk_end);
                    
                    // Merge requested range with the yt-dlp headers
                    httplib::Headers req_headers = extra_headers_;
                    req_headers.emplace("Range", std::format("bytes={}-{}", current_byte, req_end));
                    
                    bool abort_proxy = false;
                    
                    // =========================================================================
                    // UPGRADED: Two-stage HTTP GET (Status Check -> Content Stream)
                    // =========================================================================
                    auto web_res = cli.Get(path_.c_str(), req_headers, 
                        
                        // 1. RESPONSE HANDLER: Bouncer checks the status code first
                        [&](const httplib::Response& response) {
                            if (response.status != 200 && response.status != 206) {
                                write_debug_log(debug_, "[PROX] CRITICAL: CDN returned HTTP {}. Aborting stream to prevent cache poisoning.", response.status);
                                std::println(stderr, "[-] Proxy Error: CDN returned HTTP {} (Link expired or IP rate-limited)", response.status);
                                abort_proxy = true;
                                return false; // Aborts connection immediately!
                            }
                            return true;
                        },
                        
                        // 2. CONTENT RECEIVER: Only runs if the status is 200/206
                        [&](const char *data, size_t data_length) {
                            if (is_shutting_down_ || my_id < current_request_id_) {
                                abort_proxy = true;
                                return false; 
                            }
                            sink.write(data, data_length);
                            cache_.write_data(current_byte, data, data_length);
                            current_byte += data_length;
                            bytes_left -= data_length;
                            return true;
                        }
                    );

                    if (abort_proxy || !web_res) return false;
                    if (current_byte > chunk_end) cache_.set_chunk(chunk_idx);
                }
                return true; 
            }
        );
    });
}
