#pragma once
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <unordered_set>
#include <mutex>
#include <httplib.h>

class NetworkSniffer {
public:
    using SniffCallback = std::function<void(const std::string&, const httplib::Headers&)>;

    NetworkSniffer(const std::string& interface_name, SniffCallback callback);
    ~NetworkSniffer();

    void start();
    void stop();

private:
    void worker_loop();
    std::string get_keylog_path();

    std::string interface_;
    SniffCallback callback_;
    std::jthread worker_thread_;
    std::atomic<bool> active_{false};
    
    std::mutex history_mtx_;
    std::unordered_set<std::string> seen_streams_; 
};
