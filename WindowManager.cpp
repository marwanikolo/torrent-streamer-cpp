#include "WindowManager.h"
#include <algorithm>
#include <unordered_set> // <-- Added for the shield

WindowManager::WindowManager(StreamState& s, int sid) : state(s), session_id(sid) {}

WindowManager::~WindowManager() {
    update(-1, -1);
}

void WindowManager::update(int start_p, int end_p) {
    std::lock_guard<std::mutex> lk(state.mtx);

    // --- NEW: THE IN-FLIGHT SHIELD ---
    // Ask libtorrent which pieces are actively being downloaded RIGHT NOW
    auto queue = state.h.get_download_queue();
    std::unordered_set<int> in_flight;
    for (const auto& q : queue) {
        in_flight.insert(static_cast<int>(q.piece_index));
    }
    // ---------------------------------

    std::set<int> new_active;
    std::set<int> new_bg;
    
    if (start_p != -1 && end_p != -1) {
        for(int p = start_p; p <= end_p; ++p) {
            if (p >= 0 && p <= state.last_piece) new_active.insert(p);
        }
        
        // --- NEW: Dynamic Background Buffer (150 MB) ---
        int bg_buffer_bytes = 150 * 1024 * 1024;
        int bg_pieces_needed = std::max(10, bg_buffer_bytes / state.piece_length);
        
        int bg_end = std::min(state.last_piece - 1, end_p + bg_pieces_needed);
        // -----------------------------------------------
        
        for (int p = end_p + 1; p <= bg_end; ++p) {
            if (p >= 0 && p <= state.last_piece) new_bg.insert(p);
        }
    }

    auto process_removals = [&](const std::set<int>& old_set, const std::set<int>& new_set) {
        for (int p : old_set) {
            if (new_set.find(p) == new_set.end()) {
                state.piece_refs[p]--;
                if (state.piece_refs[p] <= 0) {
                    state.piece_refs.erase(p);
                    if (!state.h.have_piece(lt::piece_index_t(p)) && p != state.first_piece && p != state.last_piece) {
                        
                        // --- APPLYING THE SHIELD ---
                        if (in_flight.count(p)) {
                            // Mid-download! Soft-demote to P1. The peer will finish sending it, 
                            // keeping the connection alive without us sending an aggressive CANCEL.
                            state.h.piece_priority(lt::piece_index_t(p), lt::download_priority_t{1});
                        } else {
                            // We haven't asked anyone for this yet. Safe to hard-abort to P0.
                            state.h.piece_priority(lt::piece_index_t(p), lt::dont_download);
                        }
                        
                        state.h.reset_piece_deadline(lt::piece_index_t(p));
                    }
                }
            }
        }
    };

    auto process_additions = [&](const std::set<int>& old_set, const std::set<int>& new_set) {
        for (int p : new_set) {
            if (old_set.find(p) == old_set.end()) {
                state.piece_refs[p]++;
            }
        }
    };

    process_removals(active_window, new_active);
    process_removals(bg_window, new_bg);

    process_additions(active_window, new_active);
    process_additions(bg_window, new_bg);
    
    active_window = new_active;
    bg_window = new_bg;

    // --- THE MASTER SESSION LOGIC (Remains exactly the same) ---
    bool is_master = (session_id == state.latest_session_id.load());

    if (is_master && start_p != -1) {
        int true_playhead = start_p;

        for (auto const& [p, ref_count] : state.piece_refs) {
            if (!state.h.have_piece(lt::piece_index_t(p))) {
                
                if (active_window.find(p) != active_window.end()) {
                    int distance = p - true_playhead;
                    int prio_val = std::max(2, 7 - std::max(0, distance));
                    state.h.piece_priority(lt::piece_index_t(p), lt::download_priority_t(static_cast<uint8_t>(prio_val)));
                    
                    if (distance == 0) {
                        state.h.set_piece_deadline(lt::piece_index_t(p), 0, lt::torrent_handle::alert_when_available);
                    } else {
                        state.h.reset_piece_deadline(lt::piece_index_t(p)); 
                    }
                } 
                else if (bg_window.find(p) != bg_window.end() && ref_count == 1) {
                    if (static_cast<uint8_t>(state.h.piece_priority(lt::piece_index_t(p))) == 0) {
                        state.h.piece_priority(lt::piece_index_t(p), lt::download_priority_t{1});
                        state.h.reset_piece_deadline(lt::piece_index_t(p));
                    }
                }
                else {
                    state.h.piece_priority(lt::piece_index_t(p), lt::download_priority_t{1});
                    state.h.reset_piece_deadline(lt::piece_index_t(p)); 
                }
            }
        }
    }
}
