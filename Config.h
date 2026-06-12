#pragma once
#include <string>
#include <atomic>

// Global interrupt flag (Ctrl+C) shared across files
extern std::atomic<bool> interrupted;

struct AppConfig {
    std::string save_dir = ".";
    std::string player_path = "/usr/bin/mpv";
    int port = 9999;
    bool debug_mode = true;
    std::string gofile_token = ""; 
    
    // --- ADDED: Global header spoofing ---
    std::string custom_user_agent = "";
    std::string custom_referer = "";
};
