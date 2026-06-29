#pragma once
#include "HttpCacheManager.h"
#include "HttpDownloader.h"
#include <httplib.h>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

class HttpProxyServer {
public:
    // UPDATE: Added hls_save_dir parameter at the end
    HttpProxyServer(HttpCacheManager& cache, HttpDownloader& downloader, const std::string& video_url, int port, const std::string& stream_id, bool debug, const httplib::Headers& headers, bool is_hls = false, const std::string& hls_playlist = "", const std::vector<std::string>& hls_chunks = {}, const std::string& hls_save_dir = "");
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
    
    httplib::Headers extra_headers_;
    
    // HLS Support Variables
    bool is_hls_{false};
    std::string hls_playlist_text_;
    std::vector<std::string> hls_chunk_urls_;
    std::string hls_save_dir_; // <-- NEW: Directory for chunks
    
    httplib::Server svr_;
    std::jthread server_thread_;
    std::atomic<bool> is_shutting_down_{false};
    std::atomic<int> current_request_id_{0};
};