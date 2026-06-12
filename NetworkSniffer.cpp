#include "NetworkSniffer.h"
#include <print>
#include <iostream>
#include <sstream>
#include <vector>
#include <cstdlib>

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
        std::system("pkill -f 'tshark -l -i.*tls\\.keylog_file' 2>/dev/null");
    }
}

std::string NetworkSniffer::get_keylog_path() {
    const char* env_path = std::getenv("SSLKEYLOGFILE");
    if (env_path) return std::string(env_path);
    
    const char* home = std::getenv("HOME");
    if (home) return std::string(home) + "/chrome_tls_keys.log";
    return "";
}

void NetworkSniffer::worker_loop() {
    std::string keylog = get_keylog_path();
    if (keylog.empty()) {
        std::println(stderr, "[-] Sniffer failed: Could not locate SSLKEYLOGFILE");
        return;
    }

    // EXPANDED FILTER: Now natively captures HTTP/1.1 which many file-lockers use for raw throughput
    std::string filter = "(http2.header.value contains \"videoplayback\" || http2.header.value contains \".mp4\" || http2.header.value contains \".mkv\" || http2.header.value contains \".m3u8\" || http2.header.value contains \".webm\" || http2.header.value contains \"temp_url_sig\") || "
                         "(http3.headers.header.value contains \"videoplayback\" || http3.headers.header.value contains \".mp4\" || http3.headers.header.value contains \".m3u8\" || http3.headers.header.value contains \".webm\" || http3.headers.header.value contains \"temp_url_sig\") || "
                         "(http.request.uri contains \"videoplayback\" || http.request.uri contains \".mp4\" || http.request.uri contains \".mkv\" || http.request.uri contains \".m3u8\" || http.request.uri contains \".webm\" || http.request.uri contains \"temp_url_sig\")";

    // Injecting HTTP/1.1 specific dissector fields into the extraction arrays
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
    char buffer[4096];
    
    // Using a dynamic string buffer to ensure massive YouTube/GoFile cookies are NEVER truncated mid-read
    while (active_.load() && fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        line += buffer;
        if (line.empty() || line.back() != '\n') {
            continue; // Wait until we buffer the entire HTTP request line
        }

        std::istringstream stream(line);
        std::string protocol, h2_names, h2_vals, h3_names, h3_vals;
        std::string h1_method, h1_host, h1_uri, h1_cookie, h1_ua, h1_referer;

        // Parse all 11 columns
        std::getline(stream, protocol, '|');
        std::getline(stream, h2_names, '|');
        std::getline(stream, h2_vals, '|');
        std::getline(stream, h3_names, '|');
        std::getline(stream, h3_vals, '|');
        std::getline(stream, h1_method, '|');
        std::getline(stream, h1_host, '|');
        std::getline(stream, h1_uri, '|');
        std::getline(stream, h1_cookie, '|');
        std::getline(stream, h1_ua, '|');
        std::getline(stream, h1_referer, '|');

        std::string authority, path, method;
        httplib::Headers captured_headers;

        // Route A: Modern HTTP/2 and HTTP/3 Parsing
        if (protocol.find("HTTP2") != std::string::npos || protocol.find("HTTP3") != std::string::npos || protocol.find("QUIC") != std::string::npos) {
            std::string names = (protocol.find("HTTP2") != std::string::npos) ? h2_names : h3_names;
            std::string vals = (protocol.find("HTTP2") != std::string::npos) ? h2_vals : h3_vals;

            if (!names.empty() && !vals.empty()) {
                std::vector<std::string> name_arr, val_arr;
                std::istringstream name_stream(names), val_stream(vals);
                std::string n, v;
                while (std::getline(name_stream, n, '^')) name_arr.push_back(n);
                while (std::getline(val_stream, v, '^')) val_arr.push_back(v);

                size_t count = std::min(name_arr.size(), val_arr.size());
                for (size_t i = 0; i < count; ++i) {
                    if (name_arr[i] == ":authority") authority = val_arr[i];
                    else if (name_arr[i] == ":path") path = val_arr[i];
                    else if (name_arr[i] == ":method") method = val_arr[i];
                    else if (name_arr[i].starts_with(":")) continue;
                    else if (name_arr[i] == "accept-encoding") continue;
                    else captured_headers.emplace(name_arr[i], val_arr[i]);
                }
            }
        } 
        // Route B: Legacy HTTP/1.1 Parsing (The GoFile Route)
        else if (protocol.find("HTTP") != std::string::npos) {
            method = h1_method;
            authority = h1_host;
            path = h1_uri;
            
            auto add_h1_headers = [&](const std::string& key, const std::string& val_str) {
                if (val_str.empty()) return;
                std::istringstream vs(val_str);
                std::string v;
                while (std::getline(vs, v, '^')) captured_headers.emplace(key, v);
            };
            
            add_h1_headers("cookie", h1_cookie);
            add_h1_headers("user-agent", h1_ua);
            add_h1_headers("referer", h1_referer);
        }

        line.clear(); // Reset buffer for next packet

        if (authority.empty() || path.empty()) continue;
        
        // CRITICAL FIX: Deduplication Poisoning.
        // We only acknowledge GET requests. If we map an OPTIONS or HEAD request,
        // we lose the cookies required for the VIP bypass!
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
