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
};
