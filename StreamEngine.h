#pragma once
#include <libtorrent/session.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_handle.hpp>
#include "Config.h"
#include <memory>
#include <string>

void stream_file(lt::session& ses, AppConfig& config, lt::torrent_handle& h, 
                 std::shared_ptr<const lt::torrent_info> ti, int choice, const std::string& resume_path);
