#pragma once
#include "StreamState.h"
#include <set>
#include <mutex>

class WindowManager {
public:
    explicit WindowManager(StreamState& s, int sid); // <-- Updated signature
    ~WindowManager();

    void update(int start_p, int end_p);

private:
    StreamState& state;
    std::set<int> active_window;
    std::set<int> bg_window;
    
    // --- NEW ---
    int session_id;
    bool was_master{true};
    // -----------
};
