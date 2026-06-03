#pragma once
#include <string>
#include <sys/types.h>
#include "Config.h"

void launch_player(const AppConfig& config, const std::string& stream_url);
void stop_player();
