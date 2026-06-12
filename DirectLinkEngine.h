#pragma once
#include <string>
#include <memory>
#include <atomic>
#include <sys/types.h>
#include <httplib.h>

struct AppConfig;

struct DirectStreamHandle {
    std::string stream_id;
    pid_t player_pid;
    std::shared_ptr<std::atomic<bool>> cancel_token;
    std::shared_ptr<std::atomic<bool>> finished_token; // <-- ADD THIS
};

DirectStreamHandle stream_direct_link(AppConfig& config, const std::string& url, const httplib::Headers& headers = {}, const std::string& audio_url = "");
