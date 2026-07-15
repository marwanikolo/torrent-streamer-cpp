#include "MediaParser.h"
#include "Utils.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>

inline uint16_t read_be16(std::ifstream& f) {
    unsigned char buf[2];
    f.read(reinterpret_cast<char*>(buf), 2);
    return (buf[0] << 8) | buf[1];
}

inline uint32_t read_be32(std::ifstream& f) {
    unsigned char buf[4];
    f.read(reinterpret_cast<char*>(buf), 4);
    return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
}

inline uint64_t read_be64(std::ifstream& f) {
    unsigned char buf[8];
    f.read(reinterpret_cast<char*>(buf), 8);
    return ((uint64_t)buf[0] << 56) | ((uint64_t)buf[1] << 48) |
           ((uint64_t)buf[2] << 40) | ((uint64_t)buf[3] << 32) |
           ((uint64_t)buf[4] << 24) | ((uint64_t)buf[5] << 16) |
           ((uint64_t)buf[6] << 8)  | (uint64_t)buf[7];
}

std::vector<MapEntry> parse_clpi_file(const std::string& file_path) {
    write_debug_log(true, "[BLUR] Parsing CLPI file: {}", file_path);
    std::vector<MapEntry> master_index;
    try {
        std::ifstream f(file_path, std::ios::binary);
        if (!f) {
            write_debug_log(true, "[BLUR] Failed to open CLPI file.");
            return master_index;
        }

        f.seekg(16);
        uint32_t cpi_start_address = read_be32(f);
        if (cpi_start_address == 0) return master_index;

        f.seekg(cpi_start_address);
        read_be32(f); // skip length
        if ((read_be16(f) & 0x0F) != 1) return master_index;

        std::streampos ep_map_start_pos = f.tellg();
        uint16_t num_stream_pids = read_be16(f) & 0xFF;

        for (int i = 0; i < num_stream_pids; ++i) {
            read_be16(f); // skip pid
            
            unsigned char count_buf[6];
            f.read(reinterpret_cast<char*>(count_buf), 6);
            uint64_t packed_counts = ((uint64_t)count_buf[0] << 40) | ((uint64_t)count_buf[1] << 32) |
                                     ((uint64_t)count_buf[2] << 24) | ((uint64_t)count_buf[3] << 16) |
                                     ((uint64_t)count_buf[4] << 8)  | (uint64_t)count_buf[5];
                                     
            uint32_t num_coarse = (packed_counts >> 18) & 0xFFFF;
            uint32_t num_fine = packed_counts & 0x3FFFF;
            uint32_t start_addr = read_be32(f);

            f.seekg(ep_map_start_pos + static_cast<std::streamoff>(start_addr));
            uint32_t ep_fine_table_start_addr = read_be32(f);

            struct CoarseEntry { uint32_t ref_to_EP_fine_id; uint32_t PTS_EP_coarse; uint32_t SPN_EP_coarse; };
            struct FineEntry { uint32_t PTS_EP_fine; uint32_t SPN_EP_fine; };
            
            std::vector<CoarseEntry> coarse_table(num_coarse);
            for (uint32_t c = 0; c < num_coarse; ++c) {
                uint64_t val = read_be64(f);
                coarse_table[c] = {
                    static_cast<uint32_t>((val >> 46) & 0x3FFFF),
                    static_cast<uint32_t>((val >> 32) & 0x3FFF),
                    static_cast<uint32_t>(val & 0xFFFFFFFF)
                };
            }

            f.seekg(ep_map_start_pos + static_cast<std::streamoff>(start_addr) + static_cast<std::streamoff>(ep_fine_table_start_addr));
            std::vector<FineEntry> fine_table(num_fine);
            for (uint32_t fn = 0; fn < num_fine; ++fn) {
                uint32_t val = read_be32(f);
                fine_table[fn] = {
                    (val >> 17) & 0x7FF,
                    val & 0x1FFFF
                };
            }

            uint32_t coarse_idx = 0;
            for (uint32_t fine_idx = 0; fine_idx < fine_table.size(); ++fine_idx) {
                while (coarse_idx < coarse_table.size() - 1 && coarse_table[coarse_idx + 1].ref_to_EP_fine_id <= fine_idx) {
                    coarse_idx++;
                }
                const auto& c = coarse_table[coarse_idx];
                const auto& fn = fine_table[fine_idx];
                
                uint64_t exact_spn = (c.SPN_EP_coarse & 0xFFFE0000) + fn.SPN_EP_fine;
                uint64_t exact_pts = ((c.PTS_EP_coarse & ~1) << 19) + (fn.PTS_EP_fine << 9);
                master_index.push_back({exact_pts, exact_spn * 192});
            }
            write_debug_log(true, "[BLUR] Successfully parsed {} map entries from CLPI.", master_index.size());
            break; 
        }
    } catch (...) {
        write_debug_log(true, "[BLUR] Error encountered while parsing CLPI file.");
    }
    return master_index;
}

std::string generate_hls(const std::vector<MapEntry>& master_index, int64_t total_m2ts_size, int port) {
    write_debug_log(true, "[HLS ] Generating dynamic m3u8 playlist...");
    std::ostringstream playlist;
    double max_duration = 0.0;
    std::vector<std::string> segments;

    for (size_t i = 0; i < master_index.size(); ++i) {
        uint64_t pts = master_index[i].pts;
        uint64_t offset = master_index[i].byte_offset;
        
        uint64_t next_pts = (i + 1 < master_index.size()) ? master_index[i+1].pts : pts + 90000;
        uint64_t next_offset = (i + 1 < master_index.size()) ? master_index[i+1].byte_offset : total_m2ts_size;
        
        double duration_sec = (next_pts - pts) / 90000.0;
        int64_t length = next_offset - offset;
        
        if (length > 0 && duration_sec > 0) {
            if (duration_sec > max_duration) max_duration = duration_sec;
            std::ostringstream segment;
            segment << "#EXTINF:" << std::fixed << std::setprecision(5) << duration_sec << ",\n"
                    << "#EXT-X-BYTERANGE:" << length << "@" << offset << "\n"
                    << "http://localhost:" << port << "/stream";
            segments.push_back(segment.str());
        }
    }

    int target_duration = static_cast<int>(max_duration) + 1;
    
    playlist << "#EXTM3U\n"
             << "#EXT-X-VERSION:4\n"
             << "#EXT-X-PLAYLIST-TYPE:VOD\n"
             << "#EXT-X-TARGETDURATION:" << target_duration << "\n\n";
             
    for (const auto& seg : segments) {
        playlist << seg << "\n";
    }
    playlist << "#EXT-X-ENDLIST\n";
    
    write_debug_log(true, "[HLS ] Generated playlist with {} segments. Target duration: {}s", segments.size(), target_duration);
    return playlist.str();
}

// --- NEW: Internet HLS Proxy Rewriter ---
std::string rewrite_remote_hls(const std::string& raw_m3u8, const std::string& base_url, int port, std::vector<std::string>& out_chunk_urls) {
    // 1. Safely normalize all line endings (\r\n or \r) to standard Unix (\n)
    std::string normalized_m3u8 = raw_m3u8;
    size_t pos = 0;
    while ((pos = normalized_m3u8.find("\r\n", pos)) != std::string::npos) {
        normalized_m3u8.replace(pos, 2, "\n");
    }
    std::replace(normalized_m3u8.begin(), normalized_m3u8.end(), '\r', '\n');

    std::istringstream stream(normalized_m3u8);
    std::ostringstream rewritten;
    std::string line;
    int chunk_index = 0;

    // Helper lambda to safely build full CDN URLs
    auto resolve_url = [&](std::string full_url) {
        if (full_url.find("http") != 0) {
            if (full_url.front() == '/') {
                size_t host_end = base_url.find('/', 8); // Skip https://
                if (host_end != std::string::npos) {
                    full_url = base_url.substr(0, host_end) + full_url;
                }
            } else {
                full_url = base_url + full_url;
            }
        }
        return full_url;
    };

    // Helper lambda to extract the file extension
    auto get_extension = [](const std::string& url) {
        size_t query_pos = url.find('?');
        std::string path = (query_pos != std::string::npos) ? url.substr(0, query_pos) : url;
        size_t slash_pos = path.find_last_of('/');
        std::string filename = (slash_pos != std::string::npos) ? path.substr(slash_pos + 1) : path;
        size_t dot_pos = filename.find_last_of('.');
        if (dot_pos != std::string::npos && dot_pos != 0) {
            return filename.substr(dot_pos); // e.g., ".m4s" or ".ts"
        }
        return std::string(".ts"); // Fallback for standard MPEG-TS
    };

    while (std::getline(stream, line)) {
        if (line.empty()) {
            rewritten << "\n";
            continue;
        }

        // Intercept EXT-X-MAP to rewrite the initialization chunk URI
        if (line.starts_with("#EXT-X-MAP:")) {
            size_t uri_pos = line.find("URI=\"");
            if (uri_pos != std::string::npos) {
                size_t start = uri_pos + 5;
                size_t end = line.find("\"", start);
                
                if (end != std::string::npos) {
                    std::string raw_uri = line.substr(start, end - start);
                    std::string full_url = resolve_url(raw_uri);
                    
                    // Register the chunk with the proxy
                    out_chunk_urls.push_back(full_url);
                    
                    // Inject our local proxy route with the dynamically extracted extension
                    std::string ext = get_extension(full_url);
                    std::string new_uri = "http://localhost:" + std::to_string(port) + "/chunk/" + std::to_string(chunk_index) + ext;
                    rewritten << line.substr(0, start) << new_uri << line.substr(end) << "\n";
                    
                    chunk_index++;
                    continue; 
                }
            }
        }

        // Standard tags are passed through safely
        if (line[0] == '#') {
            rewritten << line << "\n";
        } 
        // Standard video segments are captured and replaced
        else {
            std::string full_url = resolve_url(line);
            out_chunk_urls.push_back(full_url);
            
            std::string ext = get_extension(full_url);
            rewritten << "http://localhost:" << port << "/chunk/" << chunk_index << ext << "\n";
            chunk_index++;
        }
    }
    return rewritten.str();
}
