#include "Utils.h"
#include <fstream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <mutex>

std::mutex g_log_mtx;

void write_debug_log(bool debug, const std::string& msg) {
    if (!debug) return;
    std::lock_guard<std::mutex> lk(g_log_mtx);
    std::ofstream log_file("streamer_debug.log", std::ios::app);
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    log_file << std::put_time(std::localtime(&now), "[%H:%M:%S] ") << msg << "\n";
}

bool file_exists(const std::string& name) {
    struct stat buffer;   
    return (stat(name.c_str(), &buffer) == 0); 
}

std::string get_info_hash_string(const lt::torrent_info& ti) {
    std::stringstream ss;
    ss << ti.info_hash();
    return ss.str();
}
