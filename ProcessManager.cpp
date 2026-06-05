#include "ProcessManager.h"
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <fstream> 
#include <string>  
#include <vector>

pid_t player_pid = -1;

void launch_player(const AppConfig& config, const std::string& stream_url) {
    
    // --- 1. DYNAMIC PLAYER ROUTING ---
    bool is_iso = (stream_url.find(".iso") != std::string::npos || 
                   stream_url.find(".ISO") != std::string::npos);
    
    // Force VLC if it's an ISO, otherwise use the user's preferred player
    std::string player_exec = is_iso ? "vlc" : config.player_path;

    // --- 2. GENERATE MPV LUA HOOK ---
    std::string mpv_script_path = config.save_dir + "/seek_hook.lua";
    std::ofstream mpv_lua(mpv_script_path);
    if (mpv_lua.is_open()) {
        mpv_lua << "mp.register_event(\"seek\", function()\n";
        mpv_lua << "    os.execute(\"curl -s http://localhost:" << config.port << "/abort > /dev/null &\")\n";
        mpv_lua << "end)\n";
        mpv_lua.close();
    }

    // --- 3. GENERATE VLC LUA HOOK ---
    const char* home = getenv("HOME");
    std::string vlc_dir = home ? std::string(home) + "/.local/share/vlc/lua/intf" : "/tmp/vlc_lua";
    system(("mkdir -p " + vlc_dir).c_str());
    
    std::string vlc_script_path = vlc_dir + "/tordown.lua";
    std::ofstream vlc_lua(vlc_script_path);
    if (vlc_lua.is_open()) {
        vlc_lua << "local last_time = -1\n";
        vlc_lua << "while not vlc.misc.should_die() do\n";
        vlc_lua << "    local input = vlc.object.input()\n";
        vlc_lua << "    if input then\n";
        vlc_lua << "        local current_time = vlc.var.get(input, \"time\")\n";
        vlc_lua << "        if current_time and last_time ~= -1 and math.abs(current_time - last_time) > 2000000 then\n";
        vlc_lua << "            os.execute(\"curl -s http://localhost:" << config.port << "/abort > /dev/null &\")\n";
        vlc_lua << "        end\n";
        vlc_lua << "        if current_time then last_time = current_time end\n";
        vlc_lua << "    end\n";
        vlc_lua << "    vlc.misc.mwait(vlc.misc.mdate() + 500000)\n"; 
        vlc_lua << "end\n";
        vlc_lua.close();
    }

    // --- 4. LAUNCH PLAYER ---
    player_pid = fork();
    if (player_pid == 0) {
        setpgid(0, 0);

        long max_fd = sysconf(_SC_OPEN_MAX);
        if (max_fd < 0 || max_fd > 4096) max_fd = 4096; 
        for (int i = 3; i < max_fd; ++i) close(i);

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }

        if (player_exec.find("mpv") != std::string::npos) {
            std::string script_arg = "--script=" + mpv_script_path;
            std::vector<const char*> args = {
                player_exec.c_str(),
                "--cache=yes", "--force-seekable=yes", "--network-timeout=1200", 
                "--script-opts-append=thumbfast-network=no", 
                "--demuxer-max-bytes=1024M", "--demuxer-max-back-bytes=256M",             
                script_arg.c_str(), stream_url.c_str(), nullptr 
            };
            execvp(args[0], const_cast<char* const*>(args.data()));
        } 
        else if (player_exec.find("vlc") != std::string::npos) {
            std::vector<const char*> args = {
                player_exec.c_str(),
                "--network-caching=3600000", 
                "--no-metadata-network-access", 
                "--file-caching=2000",
                "--extraintf=luaintf", 
                "--lua-intf=tordown"
            };
            
            // FIX: Disable the Java Menus for ISOs!
            if (is_iso) {
                args.push_back("--no-bluray-menu");
            }
            
            args.push_back(stream_url.c_str());
            args.push_back(nullptr);
            
            execvp(args[0], const_cast<char* const*>(args.data()));
        } 
        else {
            execlp(player_exec.c_str(), player_exec.c_str(), stream_url.c_str(), nullptr);
        }
        
        exit(1);
    }
}

void stop_player() {
    if (player_pid > 0) {
        kill(player_pid, SIGTERM);
        int status;
        pid_t res = waitpid(player_pid, &status, WNOHANG);
        
        if (res == 0) {
            usleep(1000000); 
            res = waitpid(player_pid, &status, WNOHANG);
            if (res == 0) {
                kill(player_pid, SIGKILL);
                waitpid(player_pid, &status, 0); 
            }
        }
        player_pid = -1;
    }
}
