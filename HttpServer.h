#pragma once
#include "StreamState.h"
#include "Config.h"
#include <httplib.h>
#include <string>

void run_http_server(httplib::Server& svr, StreamState& state, const std::string& hls_playlist, AppConfig& config);
