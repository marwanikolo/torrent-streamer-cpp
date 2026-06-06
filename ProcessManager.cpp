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
#include <mutex>
#include <chrono>

std::vector<pid_t> active_players;
std::mutex player_mtx;

void launch_player(const AppConfig& config, const std::string& stream_url, const std::string& abort_url, const std::string& audio_url) {
    bool is_iso = (stream_url.find(".iso") != std::string::npos || 
                   stream_url.find(".ISO") != std::string::npos);
    
    std::string player_exec = is_iso ? "vlc" : config.player_path;
    
    // Create a perfectly unique ID for the Lua script
    std::string safe_id = "default";
    if (!abort_url.empty()) {
        size_t last_slash = abort_url.find_last_of('/');
        if (last_slash != std::string::npos) safe_id = abort_url.substr(last_slash + 1);
    } else {
        safe_id = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    }

    std::string mpv_script_path = config.save_dir + "/seek_hook_" + safe_id + ".lua";
    std::string vlc_dir = std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/.local/share/vlc/lua/intf";
    std::string vlc_script_name = "tordown_" + safe_id;
    std::string vlc_script_path = vlc_dir + "/" + vlc_script_name + ".lua";

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

    pid_t pid = fork();
    if (pid == 0) {
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
            
            // --- NEW: Audio Injection ---
            std::string audio_arg = "";
            if (!audio_url.empty()) {
                audio_arg = "--audio-file=" + audio_url;
                args.push_back(audio_arg.c_str());
            }
            
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
            
            std::string lua_intf_arg = "--lua-intf=" + vlc_script_name;
            if (!abort_url.empty()) {
                args.push_back("--extraintf=luaintf");
                args.push_back(lua_intf_arg.c_str());
            }
            if (is_iso) args.push_back("--no-bluray-menu");
            
            // Note: VLC handling of external audio over HTTP is less elegant than MPV,
            // but we can append it via input-slave if necessary. MPV handles this natively.
            std::string vlc_audio_arg = "";
            if (!audio_url.empty()) {
                vlc_audio_arg = "--input-slave=" + audio_url;
                args.push_back(vlc_audio_arg.c_str());
            }
            
            args.push_back(stream_url.c_str());
            args.push_back(nullptr);
            execvp(args[0], const_cast<char* const*>(args.data()));
        } 
        else {
            execlp(player_exec.c_str(), player_exec.c_str(), stream_url.c_str(), nullptr);
        }
        exit(1);
    } else if (pid > 0) {
        std::lock_guard<std::mutex> lk(player_mtx);
        active_players.push_back(pid);
    }
}

void stop_player() {
    std::lock_guard<std::mutex> lk(player_mtx);
    for (pid_t pid : active_players) {
        kill(pid, SIGKILL); 
    }
    active_players.clear();
}
