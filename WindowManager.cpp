#include "WindowManager.h"
#include <algorithm>

WindowManager::WindowManager(StreamState& s) : state(s) {}

WindowManager::~WindowManager() {
    update(-1, -1);
}

void WindowManager::update(int start_p, int end_p) {
    std::lock_guard<std::mutex> lk(state.mtx);

    std::set<int> new_active;
    std::set<int> new_bg;

    // If start_p and end_p are valid, calculate the new desired windows
    if (start_p != -1 && end_p != -1) {
        for (int p = start_p; p <= end_p; ++p) {
            if (p >= 0 && p <= state.last_piece) new_active.insert(p);
        }
        int bg_end = std::min(state.last_piece - 1, end_p + 2000);
        for (int p = end_p + 1; p <= bg_end; ++p) {
            if (p >= 0 && p <= state.last_piece) new_bg.insert(p);
        }
    }

    // Lambda to safely decrement refs and drop priorities ONLY if no one else wants the piece
    auto process_removals = [&](const std::set<int>& old_set, const std::set<int>& new_set) {
        for (int p : old_set) {
            if (new_set.find(p) == new_set.end()) {
                state.piece_refs[p]--;
                if (state.piece_refs[p] <= 0) {
                    state.piece_refs.erase(p);
                    if (!state.h.have_piece(lt::piece_index_t(p)) && p != state.first_piece && p != state.last_piece) {
                        state.h.piece_priority(lt::piece_index_t(p), lt::dont_download);
                        state.h.reset_piece_deadline(lt::piece_index_t(p));
                    }
                }
            }
        }
    };

    // Lambda to safely increment refs for newly requested pieces
    auto process_additions = [&](const std::set<int>& old_set, const std::set<int>& new_set) {
        for (int p : new_set) {
            if (old_set.find(p) == old_set.end()) {
                state.piece_refs[p]++;
            }
        }
    };

    // 1. Process removals first to free up dead pieces
    process_removals(active_window, new_active);
    process_removals(bg_window, new_bg);

    // 2. Process additions
    process_additions(active_window, new_active);
    process_additions(bg_window, new_bg);

    // 3. Save state
    active_window = new_active;
    bg_window = new_bg;

    // 4. Apply network priorities for the pieces we want
    if (start_p != -1 && end_p != -1) {
        
        // Apply Background Buffer (Priority 1)
        for (int p : bg_window) {
            if (!state.h.have_piece(lt::piece_index_t(p))) {
                // Only set to 1 if it is currently 0 (dont accidentally downgrade active pieces from other connections!)
                if (static_cast<uint8_t>(state.h.piece_priority(lt::piece_index_t(p))) == 0) {
                    state.h.piece_priority(lt::piece_index_t(p), lt::download_priority_t{1});
                }
            }
        }

        // Apply Aggressive Sliding Window (Priority 2-7) with Deadlines
        for (int p : active_window) {
            if (!state.h.have_piece(lt::piece_index_t(p))) {
                int prio_val = std::max(2, 7 - (p - start_p));
                state.h.piece_priority(lt::piece_index_t(p), lt::download_priority_t(static_cast<uint8_t>(prio_val)));
                
                if (p <= start_p + 3) {
                    state.h.set_piece_deadline(lt::piece_index_t(p), (p - start_p) * 200, lt::torrent_handle::alert_when_available);
                } else {
                    state.h.set_piece_deadline(lt::piece_index_t(p), (p - start_p) * 1000, lt::torrent_handle::alert_when_available); 
                }
            }
        }
    }
}
