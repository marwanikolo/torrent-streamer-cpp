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

extern std::atomic<bool> interrupted;

void stream_direct_link(AppConfig& config, const std::string& url) {
    interrupted = false;
    std::println("\n[*] Initializing Direct HTTP Engine...");

    size_t protocol_pos = url.find("://");
    size_t host_start = (protocol_pos != std::string::npos) ? protocol_pos + 3 : 0;
    size_t path_start = url.find('/', host_start);

    std::string host = (path_start == std::string::npos) ? url : url.substr(0, path_start);
    std::string path = (path_start == std::string::npos) ? "/" : url.substr(path_start);

    httplib::Client cli(host);
    cli.set_follow_location(true);
    std::int64_t file_size = 0;
    auto res = cli.Head(path.c_str());

    if (res && (res->status == 200 || res->status == 206) && res->has_header("Content-Length")) {
        file_size = std::stoll(res->get_header_value("Content-Length"));
    } else {
        std::println("[*] Server blocked HEAD request. Attempting Range GET fallback...");
        httplib::Headers headers = { {"Range", "bytes=0-0"} };
        auto get_res = cli.Get(path.c_str(), headers, [&](const char*, size_t) { return false; });
        if (get_res && get_res->status == 206 && get_res->has_header("Content-Range")) {
            std::string content_range = get_res->get_header_value("Content-Range");
            size_t slash_pos = content_range.find('/');
            if (slash_pos != std::string::npos) {
                file_size = std::stoll(content_range.substr(slash_pos + 1));
            }
        }
    }

    if (file_size <= 0) {
        std::println(stderr, "[-] Failed to retrieve Content-Length. Server might not support streaming.");
        return;
    }

    std::string base_url = url;
    size_t query_pos = base_url.find('?');
    if (query_pos != std::string::npos) base_url = base_url.substr(0, query_pos);

    std::hash<std::string> hasher;
    std::string stream_id = "http_" + std::to_string(hasher(base_url));

    std::string safe_name = base_url;
    std::replace_if(safe_name.begin(), safe_name.end(), [](char c) { return !std::isalnum(c); }, '_');
    if (safe_name.length() > 50) safe_name = safe_name.substr(safe_name.length() - 50);
    std::string cache_path = config.save_dir + "/" + stream_id + "_" + safe_name + ".bin";

    auto cache = std::make_shared<HttpCacheManager>(cache_path, file_size);
    cache->init();

    auto downloader = std::make_shared<HttpDownloader>(*cache, url);
    downloader->start();

    auto proxy = std::make_shared<HttpProxyServer>(*cache, *downloader, url, config.port, stream_id, config.debug_mode);
    proxy->start();

    std::string stream_url = std::format("http://localhost:{}/stream/{}", config.port, stream_id);
    std::string abort_url = std::format("http://localhost:{}/abort/{}", config.port, stream_id);
    
    std::println("\n[*] Launching Universal HTTP Stream...");
    std::println("  => URL: {}", stream_url);
    launch_player(config, stream_url, abort_url);

    std::thread([cache, downloader, proxy]() {
        while (!interrupted.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        proxy->stop();
        downloader->stop();
        cache->save_state();
    }).detach();
}
