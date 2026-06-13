#pragma once
#include "Config.h"
#include "TorrentEngine.h"
#include "DirectLinkEngine.h"
#include "NetworkSniffer.h"
#include <httplib.h>
#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <thread>
#include <memory>
#include <mutex>

struct InterceptedStream {
    std::string url;
    httplib::Headers headers;
    std::string timestamp;
    std::int64_t size_bytes = -1;  // <-- NEW: Holds the file size
    std::string domain_tag = "";   // <-- NEW: Holds the clean [TAG]
    std::string content_type = ""; // <-- NEW: Holds the MIME type
    std::string resolution = ""; // <-- NEW: Holds the ffprobe resolution
};

class StreamerDaemon {
public:
    StreamerDaemon(const AppConfig& cfg);
    ~StreamerDaemon() = default;

    void start();
    void shutdown();

    void add_stream(const std::string& source, const AppConfig& cfg);
    void add_direct_stream(const std::string& url, const httplib::Headers& headers, const std::string& audio_url, const AppConfig& cfg);
    void stop_stream(const std::string& target);
    void list_streams();
    void inject_peer(const std::string& hash, const std::string& ip_port);

    void start_sniffer();
    void stop_sniffer();
    void list_sniffed();
    void play_sniffed(const std::vector<size_t>& indices);
    void clear_sniffed();
    std::vector<InterceptedStream> get_intercept_queue(); // <-- ADD THIS

private:
    AppConfig config_;
    TorrentManager manager_;
    httplib::Server svr_;
    std::thread alert_thread_;
    std::thread server_thread_;

    std::unordered_map<std::string, DirectStreamHandle> active_direct_streams_;
    std::shared_mutex direct_mtx_;

    std::shared_ptr<NetworkSniffer> sniffer_;
    std::vector<InterceptedStream> intercept_queue_;
    std::mutex sniff_mtx_;
};
