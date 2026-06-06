#pragma once
#include <string>
#include <unordered_map>
#include <memory>
#include <shared_mutex>
#include <vector>
#include <libtorrent/session.hpp>
#include "Config.h"
#include "StreamState.h"

class TorrentManager {
public:
    lt::session ses;
    
    // Read-Write lock for safe multi-threaded access
    std::shared_mutex registry_mtx; 
    
    // Core registry: maps stream_id (hash_index) to the stream state
    std::unordered_map<std::string, std::shared_ptr<StreamState>> active_streams;

    std::shared_ptr<StreamState> get_stream(const std::string& stream_id) {
        std::shared_lock<std::shared_mutex> lock(registry_mtx);
        auto it = active_streams.find(stream_id);
        if (it != active_streams.end()) return it->second;
        return nullptr;
    }

    std::vector<std::shared_ptr<StreamState>> get_streams_by_hash(const std::string& hash) {
        std::shared_lock<std::shared_mutex> lock(registry_mtx);
        std::vector<std::shared_ptr<StreamState>> result;
        for (const auto& [id, state] : active_streams) {
            if (id.starts_with(hash)) result.push_back(state);
        }
        return result;
    }

    void add_stream(const std::string& stream_id, std::shared_ptr<StreamState> state) {
        std::unique_lock<std::shared_mutex> lock(registry_mtx);
        active_streams[stream_id] = state;
    }

    void remove_stream(const std::string& stream_id) {
        std::unique_lock<std::shared_mutex> lock(registry_mtx);
        active_streams.erase(stream_id);
    }
};

void handle_torrent(TorrentManager& manager, AppConfig& config, std::string source);
