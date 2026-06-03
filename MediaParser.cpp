#include "MediaParser.h"
#include <fstream>
#include <sstream>
#include <iomanip>

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
    std::vector<MapEntry> master_index;
    try {
        std::ifstream f(file_path, std::ios::binary);
        if (!f) return master_index;

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
            break; 
        }
    } catch (...) {}
    return master_index;
}

std::string generate_hls(const std::vector<MapEntry>& master_index, int64_t total_m2ts_size, int port) {
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
    
    return playlist.str();
}
