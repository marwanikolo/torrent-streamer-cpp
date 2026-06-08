#pragma once
#include "StreamState.h"
#include <set>
#include <mutex>

class WindowManager {
public:
    WindowManager(StreamState& s);
    ~WindowManager();
    void update(int start_p, int end_p);
private:
    StreamState& state;
    std::set<int> active_window;
    std::set<int> bg_window;
};
