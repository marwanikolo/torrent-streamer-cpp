#pragma once
#include "TorrentEngine.h"
#include <string>

void alert_loop(TorrentManager& manager, const std::string& resume_dir, bool debug_mode);
