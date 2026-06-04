#pragma once

#include "HttpCacheManager.h"
#include "HttpDownloader.h"
#include <httplib.h>
#include <string>
#include <atomic>
#include <thread>

class HttpProxyServer {
public:
    HttpProxyServer(HttpCacheManager& cache, HttpDownloader& downloader, const std::string& video_url, int port, bool debug);
    ~HttpProxyServer();

    void start();
    void stop();

private:
    void setup_routes();

    HttpCacheManager& cache_;
    HttpDownloader& downloader_;
    
    std::string video_url_;
    std::string host_;
    std::string path_;
    int port_;
    bool debug_;

    httplib::Server svr_;
    std::thread server_thread_;
    std::atomic<bool> is_shutting_down_{false};
    
    // The core of the Zero-Latency "Active Kill" architecture
    std::atomic<int> current_request_id_{0};
};
