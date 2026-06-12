#include "NetworkSniffer.h"
#include <print>
#include <iostream>
#include <vector>
#include <cstdlib>
#include <string_view>

NetworkSniffer::NetworkSniffer(const std::string& interface_name, SniffCallback callback)
    : interface_(interface_name), callback_(std::move(callback)) {}

NetworkSniffer::~NetworkSniffer() {
    stop();
}

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
    if (const char* env_path = std::getenv("SSLKEYLOGFILE")) {
        return std::string(env_path);
    }
    if (const char* home = std::getenv("HOME")) {
        return std::string(home) + "/chrome_tls_keys.log";
    }
    return "";
}

// FIXED: Changed 'start < str.length()' to 'start <= str.length()'
// This guarantees that trailing empty fields (like an empty Referer) are captured!
static std::vector<std::string_view> split_view(std::string_view str, char delim) {
    std::vector<std::string_view> result;
    size_t start = 0;
    while (start <= str.length()) {
        size_t end = str.find(delim, start);
        if (end == std::string_view::npos) {
            result.push_back(str.substr(start));
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

    std::string filter = "(http2.header.value contains \"videoplayback\" || http2.header.value contains \".mp4\" || http2.header.value contains \".mkv\" || http2.header.value contains \".m3u8\" || http2.header.value contains \".webm\" || http2.header.value contains \"temp_url_sig\") || "
                         "(http3.headers.header.value contains \"videoplayback\" || http3.headers.header.value contains \".mp4\" || http3.headers.header.value contains \".m3u8\" || http3.headers.header.value contains \".webm\" || http3.headers.header.value contains \"temp_url_sig\") || "
                         "(http.request.uri contains \"videoplayback\" || http.request.uri contains \".mp4\" || http.request.uri contains \".mkv\" || http.request.uri contains \".m3u8\" || http.request.uri contains \".webm\" || http.request.uri contains \"temp_url_sig\")";

    std::string cmd = std::format(
        "tshark -l -i {} -o \"tls.keylog_file:{}\" -Y '{}' -T fields "
        "-e _ws.col.Protocol -e http2.header.name -e http2.header.value "
        "-e http3.header.header.name -e http3.headers.header.value "
        "-e http.request.method -e http.host -e http.request.uri -e http.cookie -e http.user_agent -e http.referer "
        "-E separator='|' -E aggregator='^' 2>/dev/null", 
        interface_, keylog, filter
    );

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return;

    std::string line;
    line.reserve(8192); 
    char buffer[4096];
    
    while (active_.load() && fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        line += buffer;
        if (line.empty() || line.back() != '\n') {
            continue; 
        }

        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }

        auto columns = split_view(line, '|');
        if (columns.size() < 11) {
            line.clear();
            continue; 
        }

        // We ignore columns[0] (_ws.col.Protocol) entirely now. It is too brittle.
        std::string_view h2_names = columns[1];
        std::string_view h2_vals  = columns[2];
        std::string_view h3_names = columns[3];
        std::string_view h3_vals  = columns[4];
        std::string_view h1_method= columns[5];
        std::string_view h1_host  = columns[6];
        std::string_view h1_uri   = columns[7];
        std::string_view h1_cookie= columns[8];
        std::string_view h1_ua    = columns[9];
        std::string_view h1_referer= columns[10];

        std::string authority, path, method;
        httplib::Headers captured_headers;

        // Route A: Modern HTTP/2 and HTTP/3 Parsing
        // FIXED: We now rely purely on the PRESENCE of HTTP2/3 headers, which is 100% accurate.
        if (!h2_names.empty() || !h3_names.empty()) {
            std::string_view names = !h2_names.empty() ? h2_names : h3_names;
            std::string_view vals  = !h2_names.empty() ? h2_vals : h3_vals;

            if (!names.empty() && !vals.empty()) {
                auto name_arr = split_view(names, '^');
                auto val_arr = split_view(vals, '^');

                size_t count = std::min(name_arr.size(), val_arr.size());
                for (size_t i = 0; i < count; ++i) {
                    if (name_arr[i] == ":authority") authority = std::string(val_arr[i]);
                    else if (name_arr[i] == ":path") path = std::string(val_arr[i]);
                    else if (name_arr[i] == ":method") method = std::string(val_arr[i]);
                    else if (name_arr[i].starts_with(":")) continue;
                    else if (name_arr[i] == "accept-encoding") continue;
                    else captured_headers.emplace(std::string(name_arr[i]), std::string(val_arr[i]));
                }
            }
        } 
        // Route B: Legacy HTTP/1.1 Parsing
        else if (!h1_method.empty()) {
            method = std::string(h1_method);
            authority = std::string(h1_host);
            path = std::string(h1_uri);
            
            auto add_h1_headers = [&](const std::string& key, std::string_view val_str) {
                if (val_str.empty()) return;
                auto vals = split_view(val_str, '^');
                for (auto v : vals) {
                    captured_headers.emplace(key, std::string(v));
                }
            };
            
            add_h1_headers("cookie", h1_cookie);
            add_h1_headers("user-agent", h1_ua);
            add_h1_headers("referer", h1_referer);
        }

        line.clear(); 

        if (authority.empty() || path.empty()) continue;
        if (method.find("GET") == std::string::npos) continue;

        bool is_media = path.find("videoplayback") != std::string::npos ||
                        path.find(".mp4") != std::string::npos ||
                        path.find(".mkv") != std::string::npos ||
                        path.find(".webm") != std::string::npos ||
                        path.find(".m3u8") != std::string::npos ||
                        path.find("temp_url_sig") != std::string::npos;
        
        if (!is_media) continue;

        std::string full_url = "https://" + authority + path;
        
        std::string dup_key;
        if (path.find("videoplayback") != std::string::npos) {
            size_t id_pos = full_url.find("id=");
            if (id_pos != std::string::npos) {
                size_t id_end = full_url.find('&', id_pos);
                dup_key = authority + path + "?" + full_url.substr(id_pos, id_end - id_pos);
            } else {
                dup_key = authority + path; 
            }
        } else {
            dup_key = full_url;
        }

        bool is_new = false;
        {
            std::lock_guard<std::mutex> lock(history_mtx_);
            
            if (seen_streams_.size() > 1000) {
                seen_streams_.clear();
            }

            if (seen_streams_.find(dup_key) == seen_streams_.end()) {
                seen_streams_.insert(dup_key);
                is_new = true;
            }
        }

        if (is_new && callback_) {
            callback_(full_url, captured_headers);
        }
    }

    pclose(pipe);
}
