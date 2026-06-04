#include "Utils.h"
#include <sys/stat.h>
#include <sstream>

// Define the global mutex declared in the header
std::mutex g_log_mtx;

bool file_exists(const std::string& name) {
    struct stat buffer;   
    return (stat(name.c_str(), &buffer) == 0); 
}

std::string get_info_hash_string(const lt::torrent_info& ti) {
    std::stringstream ss;
    ss << ti.info_hash();
    return ss.str();
}
