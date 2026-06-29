#include "DirectLinkEngine.h"
#include "HttpCacheManager.h"
#include "HttpDownloader.h"
#include "HttpProxyServer.h"
#include "ProcessManager.h"
#include "Utils.h"
#include "Config.h" 
#include "MediaParser.h" 
#include <httplib.h>
#include <print>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <atomic>
#include <filesystem>
#include <stdexcept>
#include <sstream>

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
    auto finished_token = std::make_shared<std::atomic<bool>>(false); 

    std::println("\n[*] Initializing Direct HTTP Engine...");
    write_debug_log(config.debug_mode, "[PROX] Initializing Direct HTTP Engine for target URL");

    if (next_proxy_port.load() == 0) {
        next_proxy_port.store(config.port + 1);
    }

    auto setup_proxy = [&](const std::string& target_url, const std::string& prefix) -> ProxyInstance {
        write_debug_log(config.debug_mode, "[PROX] Spinning up local proxy for {}", prefix);
        
        httplib::Headers auth_headers; 

        auto set_header = [&](const std::string& key, const std::string& value) {
            std::string clean_k = key;
            std::string clean_v = value;
            
            clean_k.erase(std::remove_if(clean_k.begin(), clean_k.end(), [](char c) { return c == '\r' || c == '\n'; }), clean_k.end());
            clean_v.erase(std::remove_if(clean_v.begin(), clean_v.end(), [](char c) { return c == '\r' || c == '\n'; }), clean_v.end());

            std::string target_lower = clean_k;
            std::transform(target_lower.begin(), target_lower.end(), target_lower.begin(), ::tolower);

            for (auto it = auth_headers.begin(); it != auth_headers.end(); ) {
                std::string current_lower = it->first;
                std::transform(current_lower.begin(), current_lower.end(), current_lower.begin(), ::tolower);
                
                if (current_lower == target_lower) {
                    it = auth_headers.erase(it); 
                } else {
                    ++it;
                }
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

        std::string final_url = target_url;
        std::string current_host, current_path;
        std::int64_t file_size = 0;
        int redirects = 0;
        int my_proxy_port = next_proxy_port++; 

        auto parse_url = [](const std::string& u, std::string& h, std::string& p) {
            size_t p_pos = u.find("://");
            size_t h_start = (p_pos != std::string::npos) ? p_pos + 3 : 0;
            size_t path_start = u.find('/', h_start);
            h = (path_start == std::string::npos) ? u : u.substr(0, path_start);
            p = (path_start == std::string::npos) ? "/" : u.substr(path_start);
        };

        parse_url(final_url, current_host, current_path);

        bool is_hls = final_url.find(".m3u8") != std::string::npos;
        std::string hls_playlist_text;
        std::vector<std::string> hls_chunk_urls;
        
        if (is_hls) {
            std::println("[*] Detected HLS Playlist. Injecting Remote Rewriter...");
            httplib::Client cli(current_host);
            cli.enable_server_certificate_verification(false); 
            auto res = cli.Get(current_path.c_str(), auth_headers);

            if (res && res->status == 200) {
                std::string m3u8_body = res->body;
                std::string base_url = final_url.substr(0, final_url.find_last_of('/') + 1);

                if (m3u8_body.find("#EXT-X-STREAM-INF") != std::string::npos) {
                    std::println("[*] Master playlist detected. Searching for max bandwidth stream...");
                    uint64_t max_bw = 0;
                    std::string best_url = "";
                    uint64_t current_bw = 0;
                    
                    std::istringstream stream(m3u8_body);
                    std::string line;
                    while (std::getline(stream, line)) {
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        
                        if (line.starts_with("#EXT-X-STREAM-INF")) {
                            size_t bw_pos = line.find("BANDWIDTH=");
                            if (bw_pos != std::string::npos) {
                                current_bw = std::stoull(line.substr(bw_pos + 10));
                            }
                        } else if (!line.empty() && line[0] != '#') {
                            if (current_bw >= max_bw) {
                                max_bw = current_bw;
                                best_url = line;
                            }
                        }
                    }

                    if (!best_url.empty()) {
                        // FIX: Securely prevent double-slashes exactly like the HLS parser
                        if (best_url.find("http") != 0) {
                            if (best_url.front() == '/') {
                                size_t host_end = base_url.find('/', 8);
                                if (host_end != std::string::npos) {
                                    best_url = base_url.substr(0, host_end) + best_url;
                                } else {
                                    best_url = base_url + best_url;
                                }
                            } else {
                                best_url = base_url + best_url;
                            }
                        }
                        
                        std::println("[*] Fetching variant playlist: {}", best_url);
                        std::string v_host, v_path;
                        parse_url(best_url, v_host, v_path);
                        
                        // FIX: Explicitly follow redirects for variant playlists
                        int v_redirects = 0;
                        while (v_redirects < 5) {
                            httplib::Client v_cli(v_host);
                            v_cli.enable_server_certificate_verification(false);
                            v_cli.set_follow_location(false); 
                            v_cli.set_connection_timeout(10);
                            v_cli.set_read_timeout(10);
                            
                            auto v_res = v_cli.Get(v_path.c_str(), auth_headers);
                            if (v_res) {
                                if (v_res->status >= 300 && v_res->status < 400 && v_res->has_header("Location")) {
                                    std::string loc = v_res->get_header_value("Location");
                                    if (loc.starts_with("http")) best_url = loc;
                                    else if (loc.starts_with("/")) best_url = v_host + loc;
                                    else best_url = base_url + loc;
                                    
                                    parse_url(best_url, v_host, v_path);
                                    base_url = best_url.substr(0, best_url.find_last_of('/') + 1);
                                    std::println("[*] Following redirect to variant host: {}", v_host);
                                    v_redirects++;
                                    continue;
                                    
                                } else if (v_res->status == 200) {
                                    std::println("[*] Successfully acquired highest quality variant playlist.");
                                    m3u8_body = v_res->body;
                                    base_url = best_url.substr(0, best_url.find_last_of('/') + 1);
                                    break;
                                } else {
                                    std::println(stderr, "[-] CDN rejected variant request. Status: {}", v_res->status);
                                    break;
                                }
                            } else {
                                std::println(stderr, "[-] Connection failed while fetching variant.");
                                break;
                            }
                        }
                    }
                }
                
                hls_playlist_text = rewrite_remote_hls(m3u8_body, base_url, my_proxy_port, hls_chunk_urls);
                file_size = 1024; 
            } else {
                throw std::runtime_error("Failed to download m3u8 playlist");
            }
        } 
        else {
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
        }

        std::string base_url = final_url;
        size_t query_pos = base_url.find('?');
        if (query_pos != std::string::npos) base_url = base_url.substr(0, query_pos);

        std::hash<std::string> hasher;
        std::string stream_id = prefix + "_" + std::to_string(hasher(base_url));

        std::string safe_name = base_url;
        std::replace_if(safe_name.begin(), safe_name.end(), [](char c) { return !std::isalnum(c); }, '_');
        if (safe_name.length() > 50) safe_name = safe_name.substr(safe_name.length() - 50);

        std::string cache_path;
        std::string hls_save_dir;

        if (is_hls) {
            hls_save_dir = config.save_dir + "/" + prefix + "_" + safe_name + "_hls";
            std::error_code ec;
            std::filesystem::create_directories(hls_save_dir, ec);
            cache_path = hls_save_dir + "/dummy_cache.bin"; 
        } else {
            cache_path = config.save_dir + "/" + prefix + "_" + safe_name + ".bin";
            
            std::error_code ec;
            if (std::filesystem::exists(cache_path, ec)) {
                std::int64_t local_size = std::filesystem::file_size(cache_path, ec);
                if (!ec && local_size != file_size) {
                    std::println(stderr, "[-] CRITICAL: Size mismatch! Local cache is {} bytes, but remote link is {} bytes.", local_size, file_size);
                    throw std::runtime_error("Size mismatch. The link has expired or points to a different file.");
                }
            }
        }

        ProxyInstance instance;
        instance.cache = std::make_shared<HttpCacheManager>(cache_path, std::max<std::int64_t>(file_size, 1024));
        instance.cache->init();

        instance.downloader = std::make_shared<HttpDownloader>(*instance.cache, final_url, auth_headers);
        
        if (!is_hls) instance.downloader->start(); 

        instance.proxy = std::make_shared<HttpProxyServer>(*instance.cache, *instance.downloader, final_url, my_proxy_port, stream_id, config.debug_mode, auth_headers, is_hls, hls_playlist_text, hls_chunk_urls, hls_save_dir);
        instance.proxy->start();

        instance.stream_url = is_hls ? std::format("http://localhost:{}/playlist.m3u8", my_proxy_port) 
                                     : std::format("http://localhost:{}/stream/{}", my_proxy_port, stream_id);
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

        std::thread([video_proxy, audio_proxy, cancel_token, finished_token]() {
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
            
            finished_token->store(true);
        }).detach();

        return { video_proxy.stream_url, pid, cancel_token, finished_token };
        
    } catch (const std::exception& e) {
        std::println(stderr, "[-] Direct HTTP Engine Aborted: {}", e.what());
        return { "failed_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()), -1, cancel_token, finished_token };
    }
}