#pragma once
#include <vector>
#include <string>
#include <cstdint>

struct MapEntry {
    uint64_t pts;
    uint64_t byte_offset;
};

std::vector<MapEntry> parse_clpi_file(const std::string& file_path);
std::string generate_hls(const std::vector<MapEntry>& master_index, int64_t total_m2ts_size, int port);

// --- NEW: Internet HLS Proxy Rewriter ---
std::string rewrite_remote_hls(const std::string& raw_m3u8, const std::string& base_url, int port, std::vector<std::string>& out_chunk_urls);