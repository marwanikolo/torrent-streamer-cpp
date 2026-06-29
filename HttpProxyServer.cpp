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

        std::string chunk_route = R"(/chunk/(\d+))";
        svr_.Get(chunk_route, [this](const httplib::Request& req, httplib::Response& res) {
            int chunk_index = 0;
            try {
                chunk_index = std::stoi(req.matches[1]);
            } catch (...) {
                res.status = 404;
                return;
            }
            
            if (chunk_index < 0 || chunk_index >= hls_chunk_urls_.size()) {
                res.status = 404;
                return;
            }

            std::string chunk_file_path = std::format("{}/chunk_{}.ts", hls_save_dir_, chunk_index);

            std::error_code ec;
            if (std::filesystem::exists(chunk_file_path, ec)) {
                std::ifstream ifs(chunk_file_path, std::ios::binary | std::ios::ate);
                if (ifs) {
                    std::streamsize size = ifs.tellg();
                    ifs.seekg(0, std::ios::beg);
                    std::string buffer(size, '\0');
                    
                    if (ifs.read(buffer.data(), size)) {
                        write_debug_log(debug_, "[PROX] HLS Cache HIT: Serving chunk {} from disk.", chunk_index);
                        res.set_content(buffer, "video/MP2T");
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

            httplib::Client cli(c_host);
            cli.set_follow_location(true);
            cli.enable_server_certificate_verification(false); 
            
            auto web_res = cli.Get(c_path.c_str(), extra_headers_);

            if (web_res && (web_res->status == 200 || web_res->status == 206)) {
                std::ofstream ofs(chunk_file_path, std::ios::binary);
                if (ofs) {
                    ofs.write(web_res->body.data(), web_res->body.size());
                }
                
                res.set_content(web_res->body, "video/MP2T");
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

                    std::int64_t req_end = std::min<std::int64_t>(current_byte + bytes_left - 1, chunk_end);
                    
                    httplib::Headers req_headers = extra_headers_;
                    req_headers.emplace("Range", std::format("bytes={}-{}", current_byte, req_end));
                    
                    bool abort_proxy = false;
                    
                    auto web_res = cli.Get(path_.c_str(), req_headers, 
                        [&](const httplib::Response& response) {
                            if (response.status != 200 && response.status != 206) {
                                write_debug_log(debug_, "[PROX] CRITICAL: CDN returned HTTP {}. Aborting stream to prevent cache poisoning.", response.status);
                                std::println(stderr, "[-] Proxy Error: CDN returned HTTP {}", response.status);
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