#pragma once
#include "StreamerDaemon.h"
#include "Config.h"

class InteractiveShell {
public:
    InteractiveShell(StreamerDaemon& daemon, const AppConfig& config);
    void run_loop();

private:
    StreamerDaemon& daemon_;
    AppConfig config_;

    // POSIX TUI Helpers
    void set_terminal_raw_mode(bool enable);
    int read_keypress();
    void launch_dashboard();
};
