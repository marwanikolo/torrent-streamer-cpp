#include "WindowManager.h"
#include <algorithm>

WindowManager::WindowManager(StreamState& s) : state(s) {}

WindowManager::~WindowManager() {
    std::lock_guard<std::mutex> lk(state.mtx);
    for (int p : active_window) {
        if (state.shutting_down.load()) continue;
        state.piece_refs[p]--;
        if (state.piece_refs[p] <= 0) {
            state.piece_refs.erase(p);
            if (state.h.is_valid() && p <= state.last_piece && !state.h.have_piece(lt::piece_index_t(p))) {
                if (p == state.first_piece || p == state.last_piece) {
                    state.h.piece_priority(lt::piece_index_t(p), lt::top_priority);
                } else {
                    state.h.piece_priority(lt::piece_index_t(p), lt::dont_download);
                    state.h.reset_piece_deadline(lt::piece_index_t(p));
                }
            }
        }
    }
}

void WindowManager::update(int start_p, int end_p) {
    std::set<int> new_window;
    for(int p = start_p; p <= end_p; ++p) {
        if (p <= state.last_piece) new_window.insert(p);
    }

    std::lock_guard<std::mutex> lk(state.mtx);

    for(int p : new_window) {
        if (active_window.find(p) == active_window.end()) {
            state.piece_refs[p]++;
        }
    }

    for(int p : active_window) {
        if (new_window.find(p) == new_window.end()) {
            state.piece_refs[p]--;
            if (state.piece_refs[p] <= 0) {
                state.piece_refs.erase(p);
                if (!state.h.have_piece(lt::piece_index_t(p))) {
                    if (p == state.first_piece || p == state.last_piece) {
                        state.h.piece_priority(lt::piece_index_t(p), lt::top_priority);
                    } else {
                        state.h.piece_priority(lt::piece_index_t(p), lt::dont_download);
                        state.h.reset_piece_deadline(lt::piece_index_t(p));
                    }
                }
            }
        }
    }
    
    active_window = new_window;

    for(int p : active_window) {
        if (!state.h.have_piece(lt::piece_index_t(p))) {
            int prio_val = std::max(1, 7 - (p - start_p));
            state.h.piece_priority(lt::piece_index_t(p), lt::download_priority_t(static_cast<uint8_t>(prio_val)));
            
            if (p <= start_p + 3) {
                state.h.set_piece_deadline(lt::piece_index_t(p), (p - start_p) * 200, lt::torrent_handle::alert_when_available);
            } else {
                state.h.set_piece_deadline(lt::piece_index_t(p), (p - start_p) * 1000, lt::torrent_handle::alert_when_available); 
            }
        }
    }
}
