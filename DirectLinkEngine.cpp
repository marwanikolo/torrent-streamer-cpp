#include "DirectLinkEngine.h"
#include "HttpCacheManager.h"
#include "HttpDownloader.h"
#include "HttpProxyServer.h"
#include "ProcessManager.h"
#include "Utils.h"
#include "Config.h" // Ensures AppConfig is correctly defined
#include <httplib.h>
#include <print>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cctype>

extern std::atomic<bool> interrupted;

void stream_direct_link(AppConfig& config, const std::string& url) {
    interrupted = false;
    std::cin.clear();

    std::println("\n[*] Initializing Direct HTTP Engine...");

    // Parse URL to isolate host and path for cpp-httplib
    size_t protocol_pos = url.find("://");
    size_t host_start = (protocol_pos != std::string::npos) ? protocol_pos + 3 : 0;
    size_t path_start = url.find('/', host_start);

    std::string host = (path_start == std::string::npos) ? url : url.substr(0, path_start);
    std::string path = (path_start == std::string::npos) ? "/" : url.substr(path_start);

    std::println("[*] Fetching file metadata from {}...", host);

    // 1. Grab the exact file size via HTTP HEAD request
    httplib::Client cli(host);
    cli.set_follow_location(true);
    
    std::int64_t file_size = 0;
    auto res = cli.Head(path.c_str());

    if (res && (res->status == 200 || res->status == 206) && res->has_header("Content-Length")) {
        file_size = std::stoll(res->get_header_value("Content-Length"));
    } 
    else {
        std::println("[*] Server blocked HEAD request. Attempting Range GET fallback...");
        
        // Fallback: Send a GET request for exactly 1 byte to force a Content-Range response
        httplib::Headers headers = { {"Range", "bytes=0-0"} };
        
        // We use a stream callback so we can instantly abort the download once we get the headers
        auto get_res = cli.Get(path.c_str(), headers,
            [&](const char*, size_t) {
                return false; // Instantly sever the socket, we only want the headers!
            }
        );

        if (get_res && get_res->status == 206 && get_res->has_header("Content-Range")) {
            std::string content_range = get_res->get_header_value("Content-Range");
            // Format is usually: "bytes 0-0/123456789"
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
    
    std::println("[*] File Size Confirmed: {} bytes", file_size);

    // 2. Generate a stable, safe filename for the .bin cache file
    // STRIP QUERY PARAMS: Ensure expiring tokens don't create new files!
    std::string base_url = url;
    size_t query_pos = base_url.find('?');
    if (query_pos != std::string::npos) {
        base_url = base_url.substr(0, query_pos);
    }

    std::string safe_name = base_url;
    std::replace_if(safe_name.begin(), safe_name.end(), [](char c) { return !std::isalnum(c); }, '_');
    if (safe_name.length() > 50) safe_name = safe_name.substr(safe_name.length() - 50);
    std::string cache_path = config.save_dir + "/http_" + safe_name + ".bin";

    // 3. Boot up our brand new C++ Direct Link Architecture
    HttpCacheManager cache(cache_path, file_size);
    cache.init();

    HttpDownloader downloader(cache, url);
    downloader.start();

    HttpProxyServer proxy(cache, downloader, url, config.port, config.debug_mode);
    proxy.start();

    // 4. Launch MPV/VLC pointed at our local C++ proxy!
    std::string launch_url = std::format("http://localhost:{}/stream", config.port);
    std::println("\n[*] Launching Universal HTTP Stream...");
    launch_player(config, launch_url);
    std::println("\n[!] STREAM ACTIVE: Press Ctrl+C to STOP and RETURN TO MENU.\n");

    while (!interrupted.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // 5. Race-condition-free graceful shutdown
    std::println("\n\n[*] Shutting down proxy engine...");
    
    stop_player();
    proxy.stop();
    downloader.stop();
    cache.save_state();

    interrupted = false;
    std::cin.clear();
}
