#include "YtdlpWrapper.h"
#include <nlohmann/json.hpp>
#include <array>
#include <memory>
#include <stdexcept>
#include <print>

using json = nlohmann::json;

std::vector<YtdlpFormat> parse_ytdlp_json(const std::string& cmd) {
    std::array<char, 256> buffer;
    std::string result;
    
    // Execute yt-dlp in the background
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        throw std::runtime_error("popen() failed! Is yt-dlp installed?");
    }
    
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    
    // FIX: Ignore pclose() because SIGCHLD is ignored in main.cpp.
    // Instead, just verify that yt-dlp actually returned data to stdout.
    pclose(pipe.release());
    
    if (result.empty()) {
        throw std::runtime_error("yt-dlp failed or returned no data. Check the URL.");
    }

    try {
        json j = json::parse(result);
        std::vector<YtdlpFormat> formats;
        
        std::string main_title = j.value("title", "yt-dlp-stream");
        
        // Extract global headers required for CDNs (like YouTube / Twitter)
        httplib::Headers global_headers;
        if (j.contains("http_headers")) {
            for (const auto& [key, value] : j["http_headers"].items()) {
                global_headers.emplace(key, value.get<std::string>());
            }
        }

        // Parse all available formats
        if (j.contains("formats") && j["formats"].is_array()) {
            for (const auto& f : j["formats"]) {
                if (!f.contains("url")) continue;
                
                YtdlpFormat fmt;
                fmt.title = main_title;
                fmt.url = f["url"].get<std::string>();
                fmt.format_id = f.value("format_id", "N/A");
                fmt.ext = f.value("ext", "unknown");
                
                // Format resolution strings safely
                if (f.contains("resolution") && f["resolution"].is_string()) {
                    fmt.resolution = f["resolution"].get<std::string>();
                } else if (f.contains("width") && f.contains("height") && !f["width"].is_null()) {
                    fmt.resolution = std::to_string(f["width"].get<int>()) + "x" + std::to_string(f["height"].get<int>());
                } else {
                    fmt.resolution = "audio-only";
                }

                fmt.vcodec = f.value("vcodec", "none");
                fmt.acodec = f.value("acodec", "none");

                // Calculate file size in MB
                double bytes = 0;
                if (f.contains("filesize") && !f["filesize"].is_null()) {
                    bytes = f["filesize"].get<double>();
                } else if (f.contains("filesize_approx") && !f["filesize_approx"].is_null()) {
                    bytes = f["filesize_approx"].get<double>();
                }
                fmt.filesize_mb = bytes / (1024.0 * 1024.0);

                // Inherit or override headers
                fmt.headers = global_headers;
                if (f.contains("http_headers")) {
                    for (const auto& [key, value] : f["http_headers"].items()) {
                        fmt.headers.emplace(key, value.get<std::string>());
                    }
                }

                formats.push_back(fmt);
            }
        } else if (j.contains("url")) {
            // Fallback for single-format streams
            YtdlpFormat fmt;
            fmt.title = main_title;
            fmt.url = j["url"].get<std::string>();
            fmt.headers = global_headers;
            fmt.format_id = j.value("format_id", "best");
            fmt.ext = j.value("ext", "mp4");
            fmt.resolution = j.value("resolution", "unknown");
            fmt.vcodec = j.value("vcodec", "unknown");
            fmt.acodec = j.value("acodec", "unknown");
            formats.push_back(fmt);
        }
        
        return formats;
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("Failed to parse yt-dlp JSON: ") + e.what());
    }
}
