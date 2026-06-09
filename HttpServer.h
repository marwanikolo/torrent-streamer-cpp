#pragma once
#include <httplib.h>
#include <string>
#include <atomic>
#include <unordered_map>
#include <shared_mutex>
#include "TorrentEngine.h"
#include "Config.h"
#include "DirectLinkEngine.h"

void run_http_server(httplib::Server& svr, 
                     TorrentManager& manager, 
                     const std::string& hls_playlist, 
                     AppConfig& config,
                     std::atomic<bool>& interrupted,
                     std::unordered_map<std::string, DirectStreamHandle>& active_direct_streams,
                     std::shared_mutex& direct_mtx);
