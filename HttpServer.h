#pragma once
#include <httplib.h>
#include <string>
#include "TorrentEngine.h"
#include "Config.h"

void run_http_server(httplib::Server& svr, TorrentManager& manager, const std::string& hls_playlist, AppConfig& config);
