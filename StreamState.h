#pragma once
#include <libtorrent/torrent_handle.hpp>
#include <string>
#include <mutex>
#include <condition_variable>
#include <map>
#include <set> // <-- Add this
#include <atomic>
#include <sys/types.h>

struct StreamState {
    lt::torrent_handle h;
    std::string file_path;
    std::int64_t file_size;
    std::int64_t file_offset;
    int piece_length;
    int num_pieces;
    int first_piece; 
    int last_piece;  
    std::mutex mtx;
    std::condition_variable cv;
    
    std::atomic<bool> shutting_down{false}; 
    std::atomic<bool> resume_data_saved{false}; 
    
    std::map<int, int> piece_refs; 
    std::atomic<int> current_request_id{0}; 
    std::atomic<int> latest_piece_requested{0}; 

    // --- NEW: Master Session Tracking ---
    std::atomic<int> latest_session_id{0};
    std::set<int> master_window; 
    // ------------------------------------

    pid_t player_pid = -1; 
};
