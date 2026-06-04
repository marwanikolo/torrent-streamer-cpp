#include "HttpCacheManager.h"
#include <print>
#include <iostream>

namespace fs = std::filesystem;

HttpCacheManager::HttpCacheManager(const std::string& base_path, std::int64_t file_size, size_t chunk_size)
    : file_path_(base_path), state_path_(base_path + ".state.bin"),
      file_size_(file_size), chunk_size_(chunk_size) {
    
    total_chunks_ = (file_size_ + chunk_size_ - 1) / chunk_size_;
    downloaded_chunks_.resize(total_chunks_, 0);
}

HttpCacheManager::~HttpCacheManager() {
    save_state();
    if (file_stream_.is_open()) {
        file_stream_.close();
    }
}

void HttpCacheManager::init() {
    std::lock_guard<std::mutex> lock(file_mtx_);
    
    bool file_exists = fs::exists(file_path_);
    
    if (!file_exists || fs::file_size(file_path_) != static_cast<uintmax_t>(file_size_)) {
        allocate_sparse_file();
    } else {
        load_state();
    }

    // Open the file for both reading and writing in binary mode without truncating
    file_stream_.open(file_path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_stream_.is_open()) {
        std::println(stderr, "[!] FATAL: Could not open cache file: {}", file_path_);
    }
}

void HttpCacheManager::allocate_sparse_file() {
    std::println("[*] Allocating sparse cache file: {} ({} bytes)", file_path_, file_size_);
    
    // Create an empty file first
    {
        std::ofstream ofs(file_path_, std::ios::binary | std::ios::trunc);
    }
    
    // C++17 native sparse allocation (Instantaneous on ext4/NTFS/APFS)
    std::error_code ec;
    fs::resize_file(file_path_, file_size_, ec);
    
    if (ec) {
        std::println(stderr, "[!] Sparse allocation failed: {}", ec.message());
    }

    // FIX: Scope the lock so it releases BEFORE we call save_state()
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        std::fill(downloaded_chunks_.begin(), downloaded_chunks_.end(), 0);
    }
    
    save_state(); 
}

void HttpCacheManager::load_state() {
    std::lock_guard<std::mutex> lock(state_mtx_);
    if (!fs::exists(state_path_)) return;

    std::ifstream ifs(state_path_, std::ios::binary);
    if (ifs.is_open()) {
        ifs.read(reinterpret_cast<char*>(downloaded_chunks_.data()), downloaded_chunks_.size());
        std::println("[*] Loaded existing HTTP cache state. Resuming...");
    }
}

void HttpCacheManager::save_state() {
    std::lock_guard<std::mutex> lock(state_mtx_);
    
    // Write to a .tmp file first, then rename. This guarantees atomic 
    // saves and prevents corruption if the user hits Ctrl+C mid-write.
    std::string temp_state = state_path_ + ".tmp";
    std::ofstream ofs(temp_state, std::ios::binary | std::ios::trunc);
    
    if (ofs.is_open()) {
        ofs.write(reinterpret_cast<const char*>(downloaded_chunks_.data()), downloaded_chunks_.size());
        ofs.close();
        
        std::error_code ec;
        fs::rename(temp_state, state_path_, ec);
    }
}

bool HttpCacheManager::has_chunk(size_t index) {
    std::lock_guard<std::mutex> lock(state_mtx_);
    if (index >= total_chunks_) return false;
    return downloaded_chunks_[index] == 1;
}

void HttpCacheManager::set_chunk(size_t index) {
    std::lock_guard<std::mutex> lock(state_mtx_);
    if (index < total_chunks_ && downloaded_chunks_[index] == 0) {
        downloaded_chunks_[index] = 1;
    }
}

size_t HttpCacheManager::get_total_chunks() const { return total_chunks_; }
size_t HttpCacheManager::get_chunk_size() const { return chunk_size_; }

bool HttpCacheManager::write_data(std::int64_t offset, const char* data, size_t length) {
    std::lock_guard<std::mutex> lock(file_mtx_);
    if (!file_stream_.is_open()) return false;

    file_stream_.seekp(offset, std::ios::beg);
    file_stream_.write(data, length);
    return file_stream_.good();
}

size_t HttpCacheManager::read_data(std::int64_t offset, char* buffer, size_t length) {
    std::lock_guard<std::mutex> lock(file_mtx_);
    if (!file_stream_.is_open()) return 0;

    file_stream_.seekg(offset, std::ios::beg);
    file_stream_.read(buffer, length);
    return file_stream_.gcount();
}
