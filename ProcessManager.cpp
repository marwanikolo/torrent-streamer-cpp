#include "ProcessManager.h"
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <fstream> 
#include <string>  

pid_t player_pid = -1;

void launch_player(const AppConfig& config, const std::string& stream_url) {
    
    // --- NEW: Generate the Active Kill Lua script dynamically ---
    // We write this to the save_dir so we don't clutter the working directory,
    // and we inject the correct dynamic port from AppConfig.
    std::string script_path = config.save_dir + "/seek_hook.lua";
    std::ofstream lua_file(script_path);
    if (lua_file.is_open()) {
        lua_file << "mp.register_event(\"seek\", function()\n";
        lua_file << "    os.execute(\"curl -s http://localhost:" << config.port << "/abort > /dev/null &\")\n";
        lua_file << "end)\n";
        lua_file.close();
    }

    player_pid = fork();
    if (player_pid == 0) {
        // 1. ISOLATE PROCESS GROUP
        // This prevents terminal signals (like pressing Ctrl+C) from hitting the 
        // media player directly. Only the C++ server will catch it, allowing 
        // us to handle the teardown in the correct order.
        setpgid(0, 0);

        // 2. PREVENT FILE DESCRIPTOR LEAKS
        // Close all inherited file descriptors from the parent.
        // If we don't do this, mpv holds the httplib server sockets hostage
        // and the parent server thread will deadlock trying to shut down.
        long max_fd = sysconf(_SC_OPEN_MAX);
        if (max_fd < 0 || max_fd > 4096) max_fd = 4096; // Sane fallback
        
        for (int i = 3; i < max_fd; ++i) {
            close(i);
        }

        // 3. SILENCE OUTPUT
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }

        if (config.player_path.find("mpv") != std::string::npos) {
            std::string script_arg = "--script=" + script_path;
            
            execlp(config.player_path.c_str(), config.player_path.c_str(), 
                   "--cache=yes", 
                   "--force-seekable=yes", 
                   "--network-timeout=1200", 
                   "--script-opts-append=thumbfast-network=no", 
                   "--demuxer-max-bytes=1024M",                 
                   "--demuxer-max-back-bytes=256M",             
                   script_arg.c_str(), // <--- Inject the Lua script here
                   stream_url.c_str(), nullptr);
        } 
        else if (config.player_path.find("vlc") != std::string::npos) {
            execlp(config.player_path.c_str(), config.player_path.c_str(),
                   "--network-caching=3600000", "--no-metadata-network-access", 
                   "--file-caching=2000", stream_url.c_str(), nullptr);
        } 
        else {
            execlp(config.player_path.c_str(), config.player_path.c_str(), stream_url.c_str(), nullptr);
        }
        exit(1);
    }
}

void stop_player() {
    if (player_pid > 0) {
        // Ask nicely first
        kill(player_pid, SIGTERM);
        
        int status;
        // WNOHANG checks if it exited without blocking our thread
        pid_t res = waitpid(player_pid, &status, WNOHANG);
        
        if (res == 0) {
            // Player is being stubborn. Wait up to 1 second for graceful exit.
            usleep(1000000); 
            res = waitpid(player_pid, &status, WNOHANG);
            
            if (res == 0) {
                // Time's up. Nuclear option.
                kill(player_pid, SIGKILL);
                waitpid(player_pid, &status, 0); 
            }
        }
        player_pid = -1;
    }
}
