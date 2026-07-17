#pragma once
#include <string>
#include <sys/types.h>
#include "Config.h"

pid_t launch_player(const AppConfig& config, const std::string& stream_url, const std::string& abort_url = "", const std::string& audio_url = "", const std::string& file_path = "");
void stop_player_by_pid(pid_t pid);
void stop_player();
