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

    auto setup_proxy = [&](const std::string& target_url, const std::string& prefix) -> ProxyInstance {
        write_debug_log(config.debug_mode, "[PROX] Spinning up local proxy for {}", prefix);
        
        httplib::Headers auth_headers; 

	// Helper to cleanly set/overwrite a header
        auto set_header = [&](const std::string& key, const std::string& value) {
            std::string clean_k = key;
            std::string clean_v = value;
            
            // FIX: Strip hidden carriage returns (\r) from tshark intercept to prevent Nginx Tarpits!
            clean_k.erase(std::remove_if(clean_k.begin(), clean_k.end(), [](char c) { return c == '\r' || c == '\n'; }), clean_k.end());
            clean_v.erase(std::remove_if(clean_v.begin(), clean_v.end(), [](char c) { return c == '\r' || c == '\n'; }), clean_v.end());

            for (auto it = auth_headers.begin(); it != auth_headers.end(); ) {
                std::string k_lower = it->first;
                std::transform(k_lower.begin(), k_lower.end(), k_lower.begin(), ::tolower);
                std::string t_lower = clean_k;
                std::transform(t_lower.begin(), t_lower.end(), t_lower.begin(), ::tolower);
                
                if (k_lower == t_lower) it = auth_headers.erase(it);
                else ++it;
            }
            auth_headers.emplace(clean_k, clean_v);
        };

        for (const auto& [k, v] : headers) set_header(k, v);
        for (const auto& [k, v] : config.custom_headers) set_header(k, v);
        
        if (!config.custom_user_agent.empty()) set_header("User-Agent", config.custom_user_agent);
        if (!config.custom_referer.empty()) set_header("Referer", config.custom_referer);

        for (auto it = auth_headers.begin(); it != auth_headers.end(); ) {
            std::string k_lower = it->first;
            std::transform(k_lower.begin(), k_lower.end(), k_lower.begin(), ::tolower);
            if (k_lower == "range" || k_lower == "host" || k_lower == "connection" || k_lower == "accept-encoding") {
                it = auth_headers.erase(it);
            } else {
                ++it;
            }
        }

        bool has_ua = false, has_ref = false, has_cookie = false;
        for (const auto& [k, v] : auth_headers) {
            std::string k_lower = k;
            std::transform(k_lower.begin(), k_lower.end(), k_lower.begin(), ::tolower);
            if (k_lower == "user-agent") has_ua = true;
            if (k_lower == "referer") has_ref = true;
            if (k_lower == "cookie") has_cookie = true;
        }

        if (target_url.find("tezfiles.com") != std::string::npos || (target_url.find("filestore.app") != std::string::npos && target_url.find("project%3Atz") != std::string::npos)) {
            std::println("[*] Detected TezFiles backend for {}. Injecting VIP headers...", prefix);
            if (!has_ua) set_header("User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/149.0.0.0 Safari/537.36");
            if (!has_ref) set_header("Referer", "https://tezfiles.com/");
        }
        else if (target_url.find("k2s.cc") != std::string::npos || target_url.find("filestore.app") != std::string::npos) {
            std::println("[*] Detected Keep2Share backend for {}. Injecting VIP headers...", prefix);
            if (!has_ua) set_header("User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/149.0.0.0 Safari/537.36");
            if (!has_ref) set_header("Referer", "https://k2s.cc/");
        }
        else if (target_url.find("gofile.io") != std::string::npos) {
            std::println("[*] Detected Gofile backend for {}. Checking auth headers...", prefix);
            if (!has_ua) set_header("User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/149.0.0.0 Safari/537.36");
            if (!has_ref) set_header("Referer", "https://gofile.io/");
            
            if (has_cookie) {
                std::println("[*] Successfully utilized dynamically intercepted Gofile session cookie!");
            } else if (!config.gofile_token.empty()) {
                set_header("Cookie", "accountToken=" + config.gofile_token);
                std::println("[*] Injected static --gofile-token from config.");
            }
        }

        std::string final_url = target_url;
        std::string current_host, current_path;
        std::int64_t file_size = 0;
        int redirects = 0;

        auto parse_url = [](const std::string& u, std::string& h, std::string& p) {
            size_t p_pos = u.find("://");
            size_t h_start = (p_pos != std::string::npos) ? p_pos + 3 : 0;
            size_t path_start = u.find('/', h_start);
            h = (path_start == std::string::npos) ? u : u.substr(0, path_start);
            p = (path_start == std::string::npos) ? "/" : u.substr(path_start);
        };

        parse_url(final_url, current_host, current_path);

        while (redirects < 5) {
            httplib::Client cli(current_host);
            cli.enable_server_certificate_verification(false); 
            cli.set_connection_timeout(5);
            cli.set_read_timeout(5);
            cli.set_follow_location(false); 

            auto res = cli.Head(current_path.c_str(), auth_headers);
            
            if (res) {
                if (res->status >= 300 && res->status < 400 && res->has_header("Location")) {
                    std::string loc = res->get_header_value("Location");
                    final_url = loc.starts_with("http") ? loc : current_host + loc;
                    parse_url(final_url, current_host, current_path);
                    std::println("[*] Following cross-domain redirect to: {}", current_host);
                    redirects++;
                    continue;
                } else if (res->status == 200 || res->status == 206) {
                    if (res->has_header("Content-Length")) {
                        file_size = std::stoll(res->get_header_value("Content-Length"));
                    }
                    break;
                } else {
                    std::println("[*] Server returned HTTP {} for HEAD check.", res->status);
                    break;
                }
            } else {
                std::println("[-] HEAD check failed.");
                break;
            }
        }

        // ======================================================================================
        // FIX: Remove the abort callback. Allow the 1-byte payload to download so we keep headers
        // ======================================================================================
        if (file_size <= 0) {
            std::println("[*] Attempting Range GET fallback...");
            httplib::Client cli(current_host);
            cli.enable_server_certificate_verification(false);
            cli.set_connection_timeout(5);
            cli.set_read_timeout(5);
            
            httplib::Headers req_headers = auth_headers; 
            req_headers.emplace("Range", "bytes=0-0");
            
            auto get_res = cli.Get(current_path.c_str(), req_headers); 
            
            if (get_res) {
                if (get_res->status == 206 && get_res->has_header("Content-Range")) {
                    std::string content_range = get_res->get_header_value("Content-Range");
                    size_t slash_pos = content_range.find('/');
                    if (slash_pos != std::string::npos) {
                        file_size = std::stoll(content_range.substr(slash_pos + 1));
                    }
                } else if (get_res->status == 200 && get_res->has_header("Content-Length")) {
                    file_size = std::stoll(get_res->get_header_value("Content-Length"));
                }
            } else {
                std::println("[-] Range GET fallback failed due to network timeout.");
            }
        }

        if (file_size <= 0) {
            std::println(stderr, "[-] Failed to retrieve Content-Length. Using dynamic cache bounds.");
            file_size = 1024;
        }

        std::string base_url = final_url;
        size_t query_pos = base_url.find('?');
        if (query_pos != std::string::npos) base_url = base_url.substr(0, query_pos);

        std::hash<std::string> hasher;
        std::string stream_id = prefix + "_" + std::to_string(hasher(base_url));

        std::string safe_name = base_url;
        std::replace_if(safe_name.begin(), safe_name.end(), [](char c) { return !std::isalnum(c); }, '_');
        if (safe_name.length() > 50) safe_name = safe_name.substr(safe_name.length() - 50);
        std::string cache_path = config.save_dir + "/" + prefix + "_" + safe_name + ".bin";

        std::error_code ec;
        if (std::filesystem::exists(cache_path, ec)) {
            std::int64_t local_size = std::filesystem::file_size(cache_path, ec);
            if (!ec && local_size != file_size) {
                std::println(stderr, "[-] CRITICAL: Size mismatch! Local cache is {} bytes, but remote link is {} bytes.", local_size, file_size);
                throw std::runtime_error("Size mismatch. The link has expired or points to a different file.");
            }
        }

        ProxyInstance instance;
        instance.cache = std::make_shared<HttpCacheManager>(cache_path, std::max<std::int64_t>(file_size, 1024));
        instance.cache->init();

        instance.downloader = std::make_shared<HttpDownloader>(*instance.cache, final_url, auth_headers);
        instance.downloader->start();

        int my_proxy_port = next_proxy_port++;

        instance.proxy = std::make_shared<HttpProxyServer>(*instance.cache, *instance.downloader, final_url, my_proxy_port, stream_id, config.debug_mode, auth_headers);
        instance.proxy->start();

        instance.stream_url = std::format("http://localhost:{}/stream/{}", my_proxy_port, stream_id);
        instance.abort_url = std::format("http://localhost:{}/abort/{}", my_proxy_port, stream_id);
        return instance;
    };

    try {
        ProxyInstance video_proxy = setup_proxy(url, "video");
        std::println("  => Video Proxy: {}", video_proxy.stream_url);

        ProxyInstance audio_proxy;
        if (!audio_url.empty()) {
            std::println("[*] Detected separate audio track. Booting secondary proxy...");
            audio_proxy = setup_proxy(audio_url, "audio");
            std::println("  => Audio Proxy: {}", audio_proxy.stream_url);
        }

        std::println("\n[*] Launching Universal HTTP Stream...");
        pid_t pid = launch_player(config, video_proxy.stream_url, video_proxy.abort_url, audio_proxy.stream_url);

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
        return { "failed_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()), -1, cancel_token };
    }
}
