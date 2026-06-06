#pragma once
#include "HttpCacheManager.h"
#include "HttpDownloader.h"
#include <httplib.h>
#include <string>
#include <thread>
#include <atomic>

class HttpProxyServer {
public:
    HttpProxyServer(HttpCacheManager& cache, HttpDownloader& downloader, const std::string& video_url, int port, const std::string& stream_id, bool debug, const httplib::Headers& headers);
    ~HttpProxyServer();
    void start();
    void stop();

private:
    void setup_routes();
    
    HttpCacheManager& cache_;
    HttpDownloader& downloader_;
    std::string video_url_;
    int port_;
    std::string stream_id_;
    bool debug_;
    
    std::string host_;
    std::string path_;
    
    // HTTP Headers for yt-dlp CDN support
    httplib::Headers extra_headers_;
    
    httplib::Server svr_;
    std::thread server_thread_;
    std::atomic<bool> is_shutting_down_{false};
    std::atomic<int> current_request_id_{0};
};
