#pragma once
#include "StreamState.h"
#include <libtorrent/session.hpp>
#include <string>

void alert_loop(lt::session& ses, StreamState* state, const std::string& resume_path, bool debug_mode);
