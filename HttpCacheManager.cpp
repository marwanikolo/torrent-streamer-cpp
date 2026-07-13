#include "HttpCacheManager.h"
#include "Utils.h"
#include <print>
#include <iostream>

namespace fs = std::filesystem;

HttpCacheManager::HttpCacheManager(const std::string& base_path, std::int64_t file_size, size_t chunk_size)
    : file_path_(base_path), state_path_(base_path + ".state.bin"),
      file_size_(file_size), chunk_size_(chunk_size) {
    
    total_chunks_ = (file_size_ + chunk_size_ - 1) / chunk_size_;
    downloaded_chunks_.resize(total_chunks_, 0);
    in_progress_chunks_.resize(total_chunks_, 0);
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

    file_stream_.open(file_path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_stream_.is_open()) {
        write_debug_log(true, "[!] FATAL: Could not open cache file: {}", file_path_);
    }
}

void HttpCacheManager::allocate_sparse_file() {
    write_debug_log(true, "[*] Allocating sparse cache file: {} ({} bytes)", file_path_, file_size_);
    
    {
        std::ofstream ofs(file_path_, std::ios::binary | std::ios::trunc);
    }
    
    std::error_code ec;
    fs::resize_file(file_path_, file_size_, ec);
    
    if (ec) {
        write_debug_log(true, "[!] Sparse allocation failed: {}", ec.message());
    }

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
        write_debug_log(true, "[*] Loaded existing HTTP cache state. Resuming...");
    }
}

void HttpCacheManager::save_state() {
    std::lock_guard<std::mutex> lock(state_mtx_);
    
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

// --- New Lock Mechanism Implementation ---

bool HttpCacheManager::try_lock_chunk(size_t index) {
    if (index >= total_chunks_) return false;
    
    // Do not lock if it's already fully downloaded
    if (has_chunk(index)) return false;

    std::lock_guard<std::mutex> lock(in_progress_mtx_);
    if (in_progress_chunks_[index] == 1) {
        return false; // Someone else is already downloading it
    }
    
    in_progress_chunks_[index] = 1;
    return true;
}

void HttpCacheManager::unlock_chunk(size_t index) {
    if (index >= total_chunks_) return;
    std::lock_guard<std::mutex> lock(in_progress_mtx_);
    in_progress_chunks_[index] = 0;
}

bool HttpCacheManager::is_chunk_in_progress(size_t index) {
    if (index >= total_chunks_) return false;
    std::lock_guard<std::mutex> lock(in_progress_mtx_);
    return in_progress_chunks_[index] == 1;
}

// --- Network Priority Mechanism Implementation ---

void HttpCacheManager::request_network_priority() {
    proxy_needs_network_.store(true);
}

void HttpCacheManager::release_network_priority() {
    proxy_needs_network_.store(false);
}

bool HttpCacheManager::is_network_preempted() const {
    return proxy_needs_network_.load();
}

// --- End New Lock Mechanism Implementation ---

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
