#include "DirectLinkEngine.h"
#include "HttpCacheManager.h"
#include "HttpDownloader.h"
#include "HttpProxyServer.h"
#include "ProcessManager.h"
#include "Utils.h"
#include <httplib.h>
#include <print>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cctype>

extern std::atomic<bool> interrupted;

// A dummy AppConfig struct definition if you don't include it via a global header.
// Make sure this matches how AppConfig is accessed in StreamEngine.cpp!
#include "Config.h" 

void stream_direct_link(AppConfig& config, const std::string& url) {
    interrupted = false;
    std::cin.clear();

    std::println("\n[*] Initializing Direct HTTP Engine...");

    size_t protocol_pos = url.find("://");
    size_t host_start = (protocol_pos != std::string::npos) ? protocol_pos + 3 : 0;
    size_t path_start = url.find('/', host_start);

    std::string host = (path_start == std::string::npos) ? url : url.substr(0, path_start);
    std::string path = (path_start == std::string::npos) ? "/" : url.substr(path_start);

    std::println("[*] Fetching file metadata from {}...", host);

    // 1. Grab the exact file size via HTTP HEAD request
    httplib::Client cli(host);
    cli.set_follow_location(true);
    auto res = cli.Head(path.c_str());

    std::int64_t file_size = 0;
    if (res && (res->status == 200 || res->status == 206)) {
        if (res->has_header("Content-Length")) {
            file_size = std::stoll(res->get_header_value("Content-Length"));
        }
    }

    if (file_size <= 0) {
        std::println(stderr, "[-] Failed to retrieve Content-Length. Server might not support streaming.");
        return;
    }

    // 2. Generate a stable, safe filename for the .bin cache file
    std::string safe_name = url;
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
