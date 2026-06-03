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
