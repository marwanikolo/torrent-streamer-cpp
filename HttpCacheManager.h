#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <fstream>
#include <cstdint>
#include <filesystem>
#include <atomic>

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

    // Download in-progress locking mechanism
    bool try_lock_chunk(size_t index);
    void unlock_chunk(size_t index);
    bool is_chunk_in_progress(size_t index);

    // Network Priority Mechanism
    void request_network_priority();
    void release_network_priority();
    bool is_network_preempted() const;

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
    std::vector<uint8_t> in_progress_chunks_;

    // Separate mutexes to prevent disk I/O, state lookups, and locks from blocking each other
    std::mutex state_mtx_;
    std::mutex file_mtx_;
    std::mutex in_progress_mtx_;

    // Atomic token to command the background thread to yield the network
    std::atomic<bool> proxy_needs_network_{false};

    // The persistent file stream
    std::fstream file_stream_;
};
