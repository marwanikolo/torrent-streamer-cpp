#include "DirectLinkEngine.h"
#include "HttpCacheManager.h"
#include "HttpDownloader.h"
#include "HttpProxyServer.h"
#include "ProcessManager.h"
#include "Utils.h"
#include "Config.h" 
#include <httplib.h>
#include <print>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <atomic>

extern std::atomic<bool> interrupted;
static std::atomic<int> next_proxy_port{0};

// Helper struct to keep proxy components alive in the background thread
struct ProxyInstance {
    std::shared_ptr<HttpCacheManager> cache;
    std::shared_ptr<HttpDownloader> downloader;
    std::shared_ptr<HttpProxyServer> proxy;
    std::string stream_url;
    std::string abort_url;
};

void stream_direct_link(AppConfig& config, const std::string& url, const httplib::Headers& headers, const std::string& audio_url) {
    interrupted = false;
    std::println("\n[*] Initializing Direct HTTP Engine...");

    if (next_proxy_port.load() == 0) {
        next_proxy_port.store(config.port + 1);
    }

    // Dynamic Proxy Generator Lambda
    auto setup_proxy = [&](const std::string& target_url, const std::string& prefix) -> ProxyInstance {
        size_t protocol_pos = target_url.find("://");
        size_t host_start = (protocol_pos != std::string::npos) ? protocol_pos + 3 : 0;
        size_t path_start = target_url.find('/', host_start);

        std::string host = (path_start == std::string::npos) ? target_url : target_url.substr(0, path_start);
        std::string path = (path_start == std::string::npos) ? "/" : target_url.substr(path_start);

        httplib::Client cli(host);
        cli.set_follow_location(true);
        std::int64_t file_size = 0;
        
        auto res = cli.Head(path.c_str(), headers);

        if (res && (res->status == 200 || res->status == 206) && res->has_header("Content-Length")) {
            file_size = std::stoll(res->get_header_value("Content-Length"));
        } else {
            if (prefix == "video") std::println("[*] Server blocked HEAD request. Attempting Range GET fallback...");
            httplib::Headers req_headers = headers; 
            req_headers.emplace("Range", "bytes=0-0");
            
            auto get_res = cli.Get(path.c_str(), req_headers, [&](const char*, size_t) { return false; });
            if (get_res && get_res->status == 206 && get_res->has_header("Content-Range")) {
                std::string content_range = get_res->get_header_value("Content-Range");
                size_t slash_pos = content_range.find('/');
                if (slash_pos != std::string::npos) {
                    file_size = std::stoll(content_range.substr(slash_pos + 1));
                }
            }
        }

        if (file_size <= 0) {
            std::println(stderr, "[-] Failed to retrieve Content-Length for {}. Using dynamic cache bounds.", prefix);
            file_size = 1024; // Provide a minimal size to allow cache initialization
        }

        std::string base_url = target_url;
        size_t query_pos = base_url.find('?');
        if (query_pos != std::string::npos) base_url = base_url.substr(0, query_pos);

        std::hash<std::string> hasher;
        std::string stream_id = prefix + "_" + std::to_string(hasher(base_url));

        std::string safe_name = base_url;
        std::replace_if(safe_name.begin(), safe_name.end(), [](char c) { return !std::isalnum(c); }, '_');
        if (safe_name.length() > 50) safe_name = safe_name.substr(safe_name.length() - 50);
        std::string cache_path = config.save_dir + "/" + prefix + "_" + safe_name + ".bin";

        ProxyInstance instance;
        instance.cache = std::make_shared<HttpCacheManager>(cache_path, std::max<std::int64_t>(file_size, 1024));
        instance.cache->init();

        instance.downloader = std::make_shared<HttpDownloader>(*instance.cache, target_url, headers);
        instance.downloader->start();

        int my_proxy_port = next_proxy_port++;

        instance.proxy = std::make_shared<HttpProxyServer>(*instance.cache, *instance.downloader, target_url, my_proxy_port, stream_id, config.debug_mode, headers);
        instance.proxy->start();

        instance.stream_url = std::format("http://localhost:{}/stream/{}", my_proxy_port, stream_id);
        instance.abort_url = std::format("http://localhost:{}/abort/{}", my_proxy_port, stream_id);
        return instance;
    };

    // Spin up the Primary Proxy (Video)
    ProxyInstance video_proxy = setup_proxy(url, "video");
    std::println("  => Video Proxy: {}", video_proxy.stream_url);

    // If an audio link was provided, spin up the Secondary Proxy (Audio)
    ProxyInstance audio_proxy;
    if (!audio_url.empty()) {
        std::println("[*] Detected separate audio track. Booting secondary proxy...");
        audio_proxy = setup_proxy(audio_url, "audio");
        std::println("  => Audio Proxy: {}", audio_proxy.stream_url);
    }

    std::println("\n[*] Launching Universal HTTP Stream...");
    launch_player(config, video_proxy.stream_url, video_proxy.abort_url, audio_proxy.stream_url);

    // Keep both proxies alive until interrupted by the daemon
    std::thread([video_proxy, audio_proxy]() {
        while (!interrupted.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        
        video_proxy.proxy->stop();
        video_proxy.downloader->stop();
        video_proxy.cache->save_state();

        if (audio_proxy.proxy) {
            audio_proxy.proxy->stop();
            audio_proxy.downloader->stop();
            audio_proxy.cache->save_state();
        }
    }).detach();
}
