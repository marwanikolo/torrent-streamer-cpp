#pragma once
#include <string>
#include <vector>
#include <httplib.h>

struct YtdlpFormat {
    std::string format_id;
    std::string ext;
    std::string resolution;
    std::string vcodec;
    std::string acodec;
    std::string url;
    httplib::Headers headers;
    std::string title;
    double filesize_mb;
};

// Intercepts the JSON and returns a list of all available streams
std::vector<YtdlpFormat> parse_ytdlp_json(const std::string& cmd);
