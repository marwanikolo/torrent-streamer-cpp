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

void launch_player(const AppConfig& config, const std::string& stream_url, const std::string& abort_url) {
    bool is_iso = (stream_url.find(".iso") != std::string::npos || 
                   stream_url.find(".ISO") != std::string::npos);
    
    std::string player_exec = is_iso ? "vlc" : config.player_path;
    std::string mpv_script_path = config.save_dir + "/seek_hook.lua";
    std::string vlc_dir = std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/.local/share/vlc/lua/intf";
    std::string vlc_script_path = vlc_dir + "/tordown.lua";

    if (!abort_url.empty()) {
        std::ofstream mpv_lua(mpv_script_path);
        if (mpv_lua.is_open()) {
            mpv_lua << "mp.register_event(\"seek\", function()\n";
            mpv_lua << "    os.execute(\"curl -s " << abort_url << " > /dev/null &\")\n";
            mpv_lua << "end)\n";
            mpv_lua.close();
        }

        system(("mkdir -p " + vlc_dir).c_str());
        std::ofstream vlc_lua(vlc_script_path);
        if (vlc_lua.is_open()) {
            vlc_lua << "local last_time = -1\n";
            vlc_lua << "while not vlc.misc.should_die() do\n";
            vlc_lua << "    local input = vlc.object.input()\n";
            vlc_lua << "    if input then\n";
            vlc_lua << "        local current_time = vlc.var.get(input, \"time\")\n";
            vlc_lua << "        if current_time and last_time ~= -1 and math.abs(current_time - last_time) > 2000000 then\n";
            vlc_lua << "            os.execute(\"curl -s " << abort_url << " > /dev/null &\")\n";
            vlc_lua << "        end\n";
            vlc_lua << "        if current_time then last_time = current_time end\n";
            vlc_lua << "    end\n";
            vlc_lua << "    vlc.misc.mwait(vlc.misc.mdate() + 500000)\n"; 
            vlc_lua << "end\n";
            vlc_lua.close();
        }
    }

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
            std::vector<const char*> args = {
                player_exec.c_str(),
                "--cache=yes", "--force-seekable=yes", "--network-timeout=1200", 
                "--script-opts-append=thumbfast-network=no", 
                "--demuxer-max-bytes=1024M", "--demuxer-max-back-bytes=256M"
            };
            
            std::string script_arg = "--script=" + mpv_script_path;
            if (!abort_url.empty()) args.push_back(script_arg.c_str());
            
            args.push_back(stream_url.c_str());
            args.push_back(nullptr);
            execvp(args[0], const_cast<char* const*>(args.data()));
        } 
        else if (player_exec.find("vlc") != std::string::npos) {
            std::vector<const char*> args = {
                player_exec.c_str(),
                "--network-caching=3600000", 
                "--no-metadata-network-access", 
                "--file-caching=2000"
            };
            
            if (!abort_url.empty()) {
                args.push_back("--extraintf=luaintf");
                args.push_back("--lua-intf=tordown");
            }
            if (is_iso) args.push_back("--no-bluray-menu");
            
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
        if (waitpid(player_pid, &status, WNOHANG) == 0) {
            usleep(1000000); 
            if (waitpid(player_pid, &status, WNOHANG) == 0) {
                kill(player_pid, SIGKILL);
                waitpid(player_pid, &status, 0); 
            }
        }
        player_pid = -1;
    }
}
