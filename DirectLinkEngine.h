#pragma once

#include <string>
#include <httplib.h>

struct AppConfig;

void stream_direct_link(AppConfig& config, const std::string& url, const httplib::Headers& headers = {}, const std::string& audio_url = "");
