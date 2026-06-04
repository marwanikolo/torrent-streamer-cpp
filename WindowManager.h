#pragma once
#include "StreamState.h"
#include <set>

struct WindowManager {
    StreamState& state;
    std::set<int> active_window;

    explicit WindowManager(StreamState& s);
    ~WindowManager();

    void update(int start_p, int end_p);
};
