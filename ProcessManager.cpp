// ProcessManager.cpp
#include "ProcessManager.h"
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <csignal>
#include <cstdlib>

pid_t player_pid = -1;

void launch_player(const AppConfig& config, const std::string& stream_url) {
    player_pid = fork();
    if (player_pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);

        if (config.player_path.find("mpv") != std::string::npos) {
            execlp(config.player_path.c_str(), config.player_path.c_str(), 
                   "--cache=yes", 
                   "--force-seekable=yes", 
                   "--network-timeout=1200", 
                   "--script-opts-append=thumbfast-network=no", // Stop thumbfast from DDoS'ing the torrent stream
                   "--demuxer-max-bytes=1024M",                 // Massive 1GB forward cache 
                   "--demuxer-max-back-bytes=256M",             // 256MB backward cache for instant rewinding
                   stream_url.c_str(), nullptr);
        } 
        // VLC specific caching arguments
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
        kill(player_pid, SIGTERM);
        waitpid(player_pid, nullptr, 0);
        player_pid = -1;
    }
}
