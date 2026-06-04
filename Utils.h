#pragma once
#include <string>
#include <libtorrent/torrent_info.hpp>

void write_debug_log(bool debug, const std::string& msg);
bool file_exists(const std::string& name);
std::string get_info_hash_string(const lt::torrent_info& ti);
