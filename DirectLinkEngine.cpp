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
#include <filesystem>
#include <stdexcept>

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

DirectStreamHandle stream_direct_link(AppConfig& config, const std::string& url, const httplib::Headers& headers, const std::string& audio_url) {
    interrupted = false;
    auto cancel_token = std::make_shared<std::atomic<bool>>(false);
    std::println("\n[*] Initializing Direct HTTP Engine...");
    write_debug_log(config.debug_mode, "[PROX] Initializing Direct HTTP Engine for target URL");

    if (next_proxy_port.load() == 0) {
        next_proxy_port.store(config.port + 1);
    }

    // Dynamic Proxy Generator Lambda
    auto setup_proxy = [&](const std::string& target_url, const std::string& prefix) -> ProxyInstance {
        write_debug_log(config.debug_mode, "[PROX] Spinning up local proxy for {}", prefix);
        
	// ====================================================================
        // DYNAMIC DOMAIN HEADER INJECTION
        // ====================================================================
        httplib::Headers auth_headers = headers; 

        bool has_custom_ua = !config.custom_user_agent.empty();
        bool has_custom_ref = !config.custom_referer.empty();

        // 0. Apply global CLI overrides first
        if (has_custom_ua) auth_headers.emplace("User-Agent", config.custom_user_agent);
        if (has_custom_ref) auth_headers.emplace("Referer", config.custom_referer);

        // 1. Check for TezFiles (Direct domain or tagged CDN node)
        if (target_url.find("tezfiles.com") != std::string::npos || 
           (target_url.find("filestore.app") != std::string::npos && target_url.find("project%3Atz") != std::string::npos)) {
            
            std::println("[*] Detected TezFiles backend for {}. Injecting VIP headers...", prefix);
            write_debug_log(config.debug_mode, "[PROX] Detected TezFiles storage node. Applying User-Agent and Referer.");
            
            // Only inject default spoofing if the user didn't provide a manual override
            if (!has_custom_ua) auth_headers.emplace("User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/149.0.0.0 Safari/537.36");
            if (!has_custom_ref) auth_headers.emplace("Referer", "https://tezfiles.com/");
        }
        // 2. Check for Keep2Share (Direct domain or fallback CDN node)
        else if (target_url.find("k2s.cc") != std::string::npos || 
                 target_url.find("filestore.app") != std::string::npos) {
            
            std::println("[*] Detected Keep2Share backend for {}. Injecting VIP headers...", prefix);
            write_debug_log(config.debug_mode, "[PROX] Detected k2s storage node. Applying User-Agent and Referer.");
            
            if (!has_custom_ua) auth_headers.emplace("User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/149.0.0.0 Safari/537.36");
            if (!has_custom_ref) auth_headers.emplace("Referer", "https://k2s.cc/");
        }
        // 3. Check for Gofile (Requires accountToken cookie and Referer)
        else if (target_url.find("gofile.io") != std::string::npos) {
            
            std::println("[*] Detected Gofile backend for {}. Injecting auth headers...", prefix);
            write_debug_log(config.debug_mode, "[PROX] Detected Gofile storage node. Applying Cookie, User-Agent, and Referer.");
            
            if (!has_custom_ua) auth_headers.emplace("User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/149.0.0.0 Safari/537.36");
            if (!has_custom_ref) auth_headers.emplace("Referer", "https://gofile.io/");
            
            if (!config.gofile_token.empty()) {
                auth_headers.emplace("Cookie", "accountToken=" + config.gofile_token);
            } else {
                std::println(stderr, "[-] Warning: Gofile link detected, but no --gofile-token was provided. The stream will likely be rejected!");
                write_debug_log(config.debug_mode, "[PROX] Warning: No Gofile token available in AppConfig.");
            }
        }
        // ====================================================================

        size_t protocol_pos = target_url.find("://");
        size_t host_start = (protocol_pos != std::string::npos) ? protocol_pos + 3 : 0;
        size_t path_start = target_url.find('/', host_start);

        std::string host = (path_start == std::string::npos) ? target_url : target_url.substr(0, path_start);
        std::string path = (path_start == std::string::npos) ? "/" : target_url.substr(path_start);

        httplib::Client cli(host);
        cli.set_follow_location(true);
        std::int64_t file_size = 0;
        
        // Apply the dynamic headers to the initial size check
        auto res = cli.Head(path.c_str(), auth_headers);

        if (res && (res->status == 200 || res->status == 206) && res->has_header("Content-Length")) {
            file_size = std::stoll(res->get_header_value("Content-Length"));
        } else {
            if (prefix == "video") {
                std::println("[*] Server blocked HEAD request. Attempting Range GET fallback...");
                write_debug_log(config.debug_mode, "[PROX] Server blocked HEAD request for {}. Attempting Range GET fallback...", prefix);
            }
            // Apply the dynamic headers to the fallback check
            httplib::Headers req_headers = auth_headers; 
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

        // ======================================================================================
        // SANITY CHECK: EXACT BYTE MATCH
        // ======================================================================================
        std::error_code ec;
        if (std::filesystem::exists(cache_path, ec)) {
            std::int64_t local_size = std::filesystem::file_size(cache_path, ec);
            
            if (!ec && local_size != file_size) {
                std::println(stderr, "[-] CRITICAL: Size mismatch! Local cache is {} bytes, but remote link is {} bytes.", local_size, file_size);
                write_debug_log(config.debug_mode, "[PROX] CRITICAL: Size mismatch (Local: {}, Remote: {}). Refusing to corrupt cache!", local_size, file_size);
                throw std::runtime_error("Size mismatch. The link has expired or points to a different file.");
            }
        }
        // ======================================================================================

        ProxyInstance instance;
        instance.cache = std::make_shared<HttpCacheManager>(cache_path, std::max<std::int64_t>(file_size, 1024));
        instance.cache->init();

        // Pass the dynamic headers down to the background caching thread
        instance.downloader = std::make_shared<HttpDownloader>(*instance.cache, target_url, auth_headers);
        instance.downloader->start();

        int my_proxy_port = next_proxy_port++;

        // Pass the dynamic headers to the local web server to handle MPV seeks
        instance.proxy = std::make_shared<HttpProxyServer>(*instance.cache, *instance.downloader, target_url, my_proxy_port, stream_id, config.debug_mode, auth_headers);
        instance.proxy->start();

        write_debug_log(config.debug_mode, "[PROX] Proxy {} ready on port {}. Cache size bounds: {}", prefix, my_proxy_port, file_size);

        instance.stream_url = std::format("http://localhost:{}/stream/{}", my_proxy_port, stream_id);
        instance.abort_url = std::format("http://localhost:{}/abort/{}", my_proxy_port, stream_id);
        return instance;
    };

    try {
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
        pid_t pid = launch_player(config, video_proxy.stream_url, video_proxy.abort_url, audio_proxy.stream_url);

        // Keep proxies alive until interrupted globally OR cancelled locally via token
        std::thread([video_proxy, audio_proxy, cancel_token]() {
            while (!interrupted.load() && !cancel_token->load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            
            video_proxy.proxy->stop();
            video_proxy.downloader->stop();
            video_proxy.cache->save_state();

            if (audio_proxy.proxy) {
                audio_proxy.proxy->stop();
                audio_proxy.downloader->stop();
                audio_proxy.cache->save_state();
            }
        }).detach();

        return { video_proxy.stream_url, pid, cancel_token };
        
    } catch (const std::exception& e) {
        std::println(stderr, "[-] Direct HTTP Engine Aborted: {}", e.what());
        // Return a dummy/invalid handle so main.cpp doesn't map a valid stream
        return { "failed_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()), -1, cancel_token };
    }
}
