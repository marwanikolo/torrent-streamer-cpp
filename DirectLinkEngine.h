#pragma once

#include <string>

// Forward declaring AppConfig assuming it is defined in your project headers
struct AppConfig;

void stream_direct_link(AppConfig& config, const std::string& url);
