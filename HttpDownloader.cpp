#include "HttpDownloader.h"
#include "Utils.h"
#include <httplib.h>
#include <print>
#include <chrono>
#include <format>
#include <algorithm>

HttpDownloader::HttpDownloader(HttpCacheManager& cache, const std::string& url, const httplib::Headers& headers) 
    : cache_(cache), url_(url), extra_headers_(headers) {
    
    size_t protocol_pos = url_.find("://");
    size_t host_start = (protocol_pos != std::string::npos) ? protocol_pos + 3 : 0;
    size_t path_start = url_.find('/', host_start);

    if (path_start == std::string::npos) {
        host_ = url_;
        path_ = "/";
    } else {
        host_ = url_.substr(0, path_start);
        path_ = url_.substr(path_start);
    }
}

HttpDownloader::~HttpDownloader() {
    stop();
}

void HttpDownloader::start() {
    if (active_.load()) return;
    active_ = true;
    worker_thread_ = std::thread(&HttpDownloader::worker_loop, this);
    write_debug_log(true, "[*] Background Downloader initialized.");
}

void HttpDownloader::stop() {
    if (!active_.load()) return;
    active_ = false;
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void HttpDownloader::update_playhead(size_t chunk_index) {
    playhead_.store(chunk_index);
}

void HttpDownloader::worker_loop() {
    int boot_wait = 0;
    while (active_.load() && boot_wait < 40) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        boot_wait++;
    }

    while (active_.load()) {
        if (cache_.is_network_preempted()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        size_t total = cache_.get_total_chunks();
        size_t target = std::string::npos;
        size_t current_playhead = playhead_.load();

        for (size_t i = current_playhead; i < total; ++i) {
            if (!cache_.has_chunk(i)) {
                target = i;
                break;
            }
        }

        if (target == std::string::npos) {
            for (size_t i = 0; i < current_playhead; ++i) {
                if (!cache_.has_chunk(i)) {
                    target = i;
                    break;
                }
            }
        }

        if (target == std::string::npos) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        if (!download_chunk(target)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    }
}

bool HttpDownloader::download_chunk(size_t chunk_index) {
    if (!cache_.try_lock_chunk(chunk_index)) {
        return false; 
    }

    httplib::Client cli(host_);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(5);
    cli.set_follow_location(true);

    std::int64_t start_byte = chunk_index * cache_.get_chunk_size();

    httplib::Headers req_headers = extra_headers_;
    
    // --- THE FIX: Never send a Range header if requesting byte 0 ---
    if (start_byte > 0) {
        req_headers.emplace("Range", std::format("bytes={}-", start_byte));
    }

    std::int64_t current_offset = start_byte;
    bool download_aborted = false;

    auto res = cli.Get(path_.c_str(), req_headers,
        [&](const char *data, size_t data_length) {
            if (!active_.load() || cache_.is_network_preempted()) {
                download_aborted = true;
                return false; 
            }
            
            cache_.write_data(current_offset, data, data_length);
            
            size_t old_chunk = current_offset / cache_.get_chunk_size();
            size_t new_chunk = (current_offset + data_length) / cache_.get_chunk_size();
            
            if (new_chunk > old_chunk) {
                cache_.set_chunk(old_chunk);
                write_debug_log(true, "[BACK] Chunk {} perfectly cached.", old_chunk);
            }

            current_offset += data_length;
            
            if (current_offset >= cache_.get_file_size()) return false;
            
            return true;
        }
    );

    if (current_offset >= cache_.get_file_size() && !download_aborted) {
        cache_.set_chunk(current_offset / cache_.get_chunk_size());
    }

    cache_.unlock_chunk(chunk_index);
    return current_offset > start_byte;
}
