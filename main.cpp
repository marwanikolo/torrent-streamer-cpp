#include <iostream>
#include <csignal>
#include <atomic>
#include <print>   // <-- ADDED
#include <cstdlib> // <-- ADDED for std::exit

#include "CliParser.h"
#include "StreamerDaemon.h"
#include "InteractiveShell.h"

// Global interrupt flag mapped to Config.h
std::atomic<bool> interrupted{false};
StreamerDaemon* global_daemon = nullptr;

void signal_handler(int) { 
    interrupted = true; 
    // Send standard interrupt to active REPL line
    if (global_daemon) {
        // If the daemon gets a hard kill (double Ctrl+C), bypass REPL and force teardown
        static int kill_count = 0;
        if (++kill_count >= 2) {
            std::println("\n[!] Emergency Teardown Triggered.");
            global_daemon->shutdown();
            std::exit(1);
        }
    }
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGCHLD, SIG_IGN);

    // 1. Parse Arguments & Environment Variables
    AppConfig config = parse_cli_args(argc, argv);

    // 2. Initialize the Core Engine
    StreamerDaemon daemon(config);
    global_daemon = &daemon;
    daemon.start();

    // 3. Trigger Auto-Launch (If URL provided via CLI)
    if (!config.initial_source.empty()) {
        daemon.add_stream(config.initial_source, config);
    }

    // 4. Run Interactive Prompt
    InteractiveShell shell(daemon, config);
    shell.run_loop();

    // 5. Graceful Engine Teardown
    daemon.shutdown();
    return 0;
}
