#include "NetworkSniffer.h"
#include <print>
#include <iostream>
#include <vector>
#include <cstdlib>
#include <string_view>
#include <mutex>
#include <filesystem>

NetworkSniffer::NetworkSniffer(const std::string& interface_name, SniffCallback callback)
    : interface_(interface_name), callback_(std::move(callback)) {}

NetworkSniffer::~NetworkSniffer() { stop(); }

void NetworkSniffer::start() {
    if (active_.load()) return;
    active_ = true;
    worker_thread_ = std::jthread(&NetworkSniffer::worker_loop, this);
    std::println("[*] Native Network Sniffer started on interface: {}", interface_);
}

void NetworkSniffer::stop() {
    if (active_.exchange(false)) {
        std::println("[*] Severing tshark network pipe...");
        std::string keylog = get_keylog_path();
        std::string kill_cmd = std::format("pkill -f 'tshark.*-i {}.*{}' 2>/dev/null", interface_, keylog);
        std::system(kill_cmd.c_str());
    }
}

std::string NetworkSniffer::get_keylog_path() {
    if (const char* env_path = std::getenv("SSLKEYLOGFILE")) return std::string(env_path);
    if (const char* home = std::getenv("HOME")) return std::string(home) + "/chrome_tls_keys.log";
    return "";
}

static std::vector<std::string_view> split_view(std::string_view str, char delim) {
    std::vector<std::string_view> result;
    size_t start = 0;
    while (start <= str.length()) {
        size_t end = str.find(delim, start);
        if (end == std::string_view::npos) {
            if (start < str.length()) result.push_back(str.substr(start));
            break;
        }
        result.push_back(str.substr(start, end - start));
        start = end + 1;
    }
    return result;
}

void NetworkSniffer::worker_loop() {
    std::string keylog = get_keylog_path();
    if (keylog.empty()) {
        std::println(stderr, "[-] Sniffer failed: Could not locate SSLKEYLOGFILE");
        return;
    }

    if (!std::filesystem::exists("scripts/daemon_sniffer.lua")) {
        std::println(stderr, "[-] Sniffer failed: Could not find scripts/daemon_sniffer.lua");
        return;
    }

    // -q suppresses all normal tshark output. -X lua_script attaches our engine.
    std::string cmd = std::format(
        "tshark -q -l -i {} -o \"tls.keylog_file:{}\" -X lua_script:scripts/daemon_sniffer.lua 2>/dev/null", 
        interface_, keylog
    );

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return;

    char buffer[8192];
    const std::string_view hook_prefix = "[DAEMON_HOOK] ";
    
    while (active_.load() && fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::string_view line(buffer);
        
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.remove_suffix(1);
        }

        if (!line.starts_with(hook_prefix)) continue; 
        
        line.remove_prefix(hook_prefix.length());

        size_t delim_pos = line.find('|');
        if (delim_pos == std::string_view::npos) continue;

        std::string full_url(line.substr(0, delim_pos));
        std::string_view header_chunk = line.substr(delim_pos + 1);

        // Deduplication Logic
        std::string dup_key = full_url;
        if (full_url.find("videoplayback") != std::string::npos) {
            size_t id_pos = full_url.find("id=");
            if (id_pos != std::string::npos) {
                size_t id_end = full_url.find('&', id_pos);
                dup_key = full_url.substr(0, full_url.find('?')) + "?" + full_url.substr(id_pos, id_end - id_pos);
            }
        }

        bool is_new = false;
        {
            std::lock_guard<std::mutex> lock(history_mtx_);
            if (seen_streams_.size() > 1000) seen_streams_.clear();
            if (seen_streams_.insert(dup_key).second) {
                is_new = true;
            }
        }

        if (is_new && callback_) {
            httplib::Headers captured_headers;
            
            auto header_pairs = split_view(header_chunk, '^');
            for (auto pair : header_pairs) {
                size_t eq_pos = pair.find('=');
                if (eq_pos != std::string_view::npos) {
                    captured_headers.emplace(
                        std::string(pair.substr(0, eq_pos)),
                        std::string(pair.substr(eq_pos + 1))
                    );
                }
            }

            callback_(full_url, captured_headers);
        }
    }

    pclose(pipe);
}
