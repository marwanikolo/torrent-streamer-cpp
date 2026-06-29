#include "Utils.h"
#include <sys/stat.h>
#include <sstream>
#include <fstream>

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

std::vector<std::pair<std::string, std::string>> parse_burp_file(const std::string& filepath) {
    std::vector<std::pair<std::string, std::string>> headers;
    std::ifstream file(filepath);
    
    if (!file.is_open()) {
        std::println(stderr, "[-] Error: Could not open Burp request file '{}'", filepath);
        return headers;
    }

    std::string line;
    bool is_first_line = true;

    while (std::getline(file, line)) {
        // Burp Suite copies with Windows CRLF (\r\n). Strip the \r.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // Skip the initial "GET /path HTTP/1.1" line
        if (is_first_line) {
            is_first_line = false;
            continue;
        }

        // An empty line in HTTP signifies the end of the headers
        if (line.empty()) {
            break; 
        }

        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);
            
            // Trim leading whitespace from the value
            size_t start = value.find_first_not_of(" \t");
            if (start != std::string::npos) {
                value = value.substr(start);
            } else {
                value = "";
            }
            
            // Skip the "Host" header so your HTTP client auto-generates it based on the URL
            if (key != "Host") {
                headers.push_back({key, value});
            }
        }
    }
    return headers;
}