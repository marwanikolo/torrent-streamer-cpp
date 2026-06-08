#include "WindowManager.h"
#include <algorithm>

// FIX: Added 'int sid' to match the header, even though the new global playhead handles everything natively!
WindowManager::WindowManager(StreamState& s, int sid) : state(s) {}

WindowManager::~WindowManager() {
    // When an HTTP thread dies, cleanly unregister all its pieces
    update(-1, -1);
}

void WindowManager::update(int start_p, int end_p) {
    std::lock_guard<std::mutex> lk(state.mtx);

    std::set<int> new_window;
    
    // 1. Calculate the new desired window for this specific HTTP thread
    if (start_p != -1 && end_p != -1) {
        for(int p = start_p; p <= end_p; ++p) {
            if (p >= 0 && p <= state.last_piece) new_window.insert(p);
        }
        
        // Handle the passive background buffer independently
        int bg_end = std::min(state.last_piece - 1, end_p + 2000);
        for (int p = end_p + 1; p <= bg_end; ++p) {
            if (p >= 0 && p <= state.last_piece && !state.h.have_piece(lt::piece_index_t(p))) {
                // Background pieces are strictly Priority 1
                if (static_cast<uint8_t>(state.h.piece_priority(lt::piece_index_t(p))) == 0) {
                    state.h.piece_priority(lt::piece_index_t(p), lt::download_priority_t{1});
                }
            }
        }
    }

    // 2. Add new pieces to the global reference counter
    for(int p : new_window) {
        if (active_window.find(p) == active_window.end()) {
            state.piece_refs[p]++;
        }
    }

    // 3. Remove old pieces from the global reference counter
    for(int p : active_window) {
        if (new_window.find(p) == new_window.end()) {
            state.piece_refs[p]--;
            if (state.piece_refs[p] <= 0) {
                state.piece_refs.erase(p);
                // If NO threads want this piece anymore, drop it to 0
                if (!state.h.have_piece(lt::piece_index_t(p)) && p != state.first_piece && p != state.last_piece) {
                    state.h.piece_priority(lt::piece_index_t(p), lt::dont_download);
                    state.h.reset_piece_deadline(lt::piece_index_t(p));
                }
            }
        }
    }
    
    active_window = new_window;

    // --- THE GLOBAL SLIDING WINDOW ---
    // Find the absolute earliest missing piece across ALL concurrent MPV threads
    int global_playhead = -1;
    for (auto const& [p, ref_count] : state.piece_refs) {
        if (!state.h.have_piece(lt::piece_index_t(p))) {
            global_playhead = p;
            break; // std::map is natively sorted from lowest to highest!
        }
    }

    // Apply the strict p7 -> p6 -> p5 cascade based on the true playhead
    if (global_playhead != -1) {
        for(auto const& [p, ref_count] : state.piece_refs) {
            if (!state.h.have_piece(lt::piece_index_t(p))) {
                
                int distance = p - global_playhead;
                
                // Active HTTP requests hit a minimum of Priority 2. 
                int prio_val = std::max(2, 7 - distance);
                state.h.piece_priority(lt::piece_index_t(p), lt::download_priority_t(static_cast<uint8_t>(prio_val)));
                
                // CRITICAL FIX: Deadlines internally force priority to 7 in libtorrent!
                // We ONLY set a deadline for the exact piece the player is currently waiting for.
                if (distance == 0) {
                    state.h.set_piece_deadline(lt::piece_index_t(p), 0, lt::torrent_handle::alert_when_available);
                } else {
                    // We MUST clear deadlines for future pieces so libtorrent respects our p6, p5, p4 math natively!
                    state.h.reset_piece_deadline(lt::piece_index_t(p)); 
                }
            }
        }
    }
}
