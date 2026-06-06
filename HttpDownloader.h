#pragma once

#include "HttpCacheManager.h"
#include <string>
#include <thread>
#include <atomic>
#include <httplib.h>

class HttpDownloader {
public:
    HttpDownloader(HttpCacheManager& cache, const std::string& url, const httplib::Headers& headers);
    ~HttpDownloader();

    void start();
    void stop();
    
    // Allows the live proxy server to steer the background worker
    void update_playhead(size_t chunk_index);

private:
    void worker_loop();
    bool download_chunk(size_t chunk_index);

    HttpCacheManager& cache_;
    
    // URL parsing state
    std::string url_;
    std::string host_;
    std::string path_;
    
    // HTTP Headers for yt-dlp CDN support
    httplib::Headers extra_headers_;

    // Threading state
    std::thread worker_thread_;
    std::atomic<bool> active_{false};
    std::atomic<size_t> playhead_{0};
};
