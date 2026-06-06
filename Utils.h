#pragma once
#include <string>
#include <libtorrent/torrent_info.hpp>
#include <mutex>
#include <fstream>
#include <print>
#include <format>
#include <chrono>
#include <ctime>

extern std::mutex g_log_mtx;

// ---------------------------------------------------------
// LEGACY OVERLOAD (Protects existing std::format calls)
// ---------------------------------------------------------
inline void write_debug_log(bool debug, const std::string& msg) {
    if (!debug) return;

    std::lock_guard<std::mutex> lk(g_log_mtx);
    std::ofstream log_file("streamer_debug.log", std::ios::app);

    // Thread-safe time extraction
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm_buf;
    localtime_r(&now, &tm_buf); 

    // REMOVED std::println so background threads stop polluting the daemon prompt!
    
    // Write exclusively to the log file
    log_file << std::format("[{:02}:{:02}:{:02}] {}\n", tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, msg);
}

// ---------------------------------------------------------
// C++23 VARIADIC LOGGER (The new, clean way)
// ---------------------------------------------------------
// Enables: write_debug_log(debug, "Session ID: {} | Bytes: {}", id, bytes);
template <typename... Args>
inline void write_debug_log(bool debug, std::format_string<Args...> fmt, Args&&... args) {
    if (!debug) return;
    write_debug_log(debug, std::format(fmt, std::forward<Args>(args)...));
}

bool file_exists(const std::string& name);
std::string get_info_hash_string(const lt::torrent_info& ti);
