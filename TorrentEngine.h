#pragma once
#include <string>
#include <libtorrent/session.hpp>
#include "Config.h"

void handle_torrent(lt::session& ses, AppConfig& config, std::string source);
