#include "YtdlpWrapper.h"
#include <nlohmann/json.hpp>
#include <array>
#include <memory>
#include <stdexcept>
#include <print>
#include <sstream>

using json = nlohmann::json;

YtdlpResult parse_ytdlp_json(const std::string& cmd) {
    std::array<char, 256> buffer;
    std::string result;

    std::unique_ptr<FILE, void(*)(FILE*)> pipe(popen(cmd.c_str(), "r"), [](FILE* p) { 
        if (p) pclose(p); 
    });

    if (!pipe) throw std::runtime_error("popen() failed! Is yt-dlp installed?");
    
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) result += buffer.data();
    pclose(pipe.release());
    
    if (result.empty()) throw std::runtime_error("yt-dlp failed or returned no data. Check the URL.");

    YtdlpResult res;
    try {
        // Handle JSONL (Multiple JSON objects separated by newlines, used in Search Results)
        std::vector<json> root_objects;
        std::istringstream iss(result);
        std::string line;
        while(std::getline(iss, line)) {
            if(line.empty()) continue;
            try { root_objects.push_back(json::parse(line)); } catch(...) {}
        }

        if (root_objects.empty()) throw std::runtime_error("Failed to parse JSON.");

        // If multiple objects returned, it's a search result / flat playlist
        if (root_objects.size() > 1) {
            res.is_playlist = true;
            for (const auto& j : root_objects) {
                res.entries.push_back({j.value("title", "Unknown"), j.value("url", j.value("webpage_url", ""))});
            }
            return res;
        }

        json j = root_objects[0];

        // Standard Playlist Object handling
        if (j.contains("_type") && (j["_type"] == "playlist" || j["_type"] == "multi_video")) {
            res.is_playlist = true;
            if (j.contains("entries")) {
                for (const auto& e : j["entries"]) {
                    if (e.is_null()) continue;
                    res.entries.push_back({e.value("title", "Unknown"), e.value("url", e.value("webpage_url", ""))});
                }
            }
            return res;
        }

        // Single Video Format Parsing
        std::string main_title = j.value("title", "yt-dlp-stream");
        httplib::Headers global_headers;
        if (j.contains("http_headers")) {
            for (const auto& [key, value] : j["http_headers"].items()) global_headers.emplace(key, value.get<std::string>());
        }

        if (j.contains("formats") && j["formats"].is_array()) {
            for (const auto& f : j["formats"]) {
                if (!f.contains("url")) continue;
                YtdlpFormat fmt;
                fmt.title = main_title;
                fmt.url = f["url"].get<std::string>();
                fmt.format_id = f.value("format_id", "N/A");
                fmt.ext = f.value("ext", "unknown");
                
                if (f.contains("resolution") && f["resolution"].is_string()) fmt.resolution = f["resolution"].get<std::string>();
                else if (f.contains("width") && f.contains("height") && !f["width"].is_null()) fmt.resolution = std::to_string(f["width"].get<int>()) + "x" + std::to_string(f["height"].get<int>());
                else fmt.resolution = "audio-only";

                fmt.vcodec = f.value("vcodec", "none");
                fmt.acodec = f.value("acodec", "none");

                double bytes = 0;
                if (f.contains("filesize") && !f["filesize"].is_null()) bytes = f["filesize"].get<double>();
                else if (f.contains("filesize_approx") && !f["filesize_approx"].is_null()) bytes = f["filesize_approx"].get<double>();
                fmt.filesize_mb = bytes / (1024.0 * 1024.0);

                fmt.headers = global_headers;
                if (f.contains("http_headers")) {
                    for (const auto& [key, value] : f["http_headers"].items()) fmt.headers.emplace(key, value.get<std::string>());
                }
                res.formats.push_back(fmt);
            }
        }
        return res;
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("Failed to parse yt-dlp JSON: ") + e.what());
    }
}
