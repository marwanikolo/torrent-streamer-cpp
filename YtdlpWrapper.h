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

struct YtdlpPlaylistEntry {
    std::string title;
    std::string url;
};

struct YtdlpResult {
    bool is_playlist = false;
    std::vector<YtdlpFormat> formats;
    std::vector<YtdlpPlaylistEntry> entries;
};

YtdlpResult parse_ytdlp_json(const std::string& cmd);
