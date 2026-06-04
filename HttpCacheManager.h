#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <fstream>
#include <cstdint>
#include <filesystem>

class HttpCacheManager {
public:
    // Defaults to 1MB chunks to match the Node.js architecture
    HttpCacheManager(const std::string& base_path, std::int64_t file_size, size_t chunk_size = 1024 * 1024);
    ~HttpCacheManager();

    // Core initialization
    void init();

    // Chunk tracking
    bool has_chunk(size_t index);
    void set_chunk(size_t index);
    size_t get_total_chunks() const;
    size_t get_chunk_size() const;

    // Thread-safe disk I/O
    bool write_data(std::int64_t offset, const char* data, size_t length);
    size_t read_data(std::int64_t offset, char* buffer, size_t length);

    // State persistence
    void save_state();

    std::int64_t get_file_size() const { return file_size_; }

private:
    void load_state();
    void allocate_sparse_file();

    std::string file_path_;
    std::string state_path_;
    std::int64_t file_size_;
    size_t chunk_size_;
    size_t total_chunks_;

    // Using uint8_t instead of vector<bool> because vector<bool> returns 
    // proxy objects in C++ which makes thread-safety very messy.
    std::vector<uint8_t> downloaded_chunks_;

    // Two separate mutexes to prevent disk I/O from blocking quick state lookups
    std::mutex state_mtx_;
    std::mutex file_mtx_;

    // The persistent file stream
    std::fstream file_stream_;
};
