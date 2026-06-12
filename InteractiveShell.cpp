#include "InteractiveShell.h"
#include "YtdlpWrapper.h"
#include <iostream>
#include <print>
#include <string>
#include <vector>

InteractiveShell::InteractiveShell(StreamerDaemon& daemon, const AppConfig& config)
    : daemon_(daemon), config_(config) {}

void InteractiveShell::run_loop() {
    std::println("\nCommands:");
    std::println("  add <link>              - Add a new magnet, .torrent, or HTTP link");
    std::println("  stop <url>              - Stop a specific running stream");
    std::println("  yt                      - Enter interactive yt-dlp sub-shell");
    std::println("  sniff start             - Start network interception queue");
    std::println("  sniff list              - Show captured media streams");
    std::println("  sniff play <idx>        - Play an intercepted stream");
    std::println("  sniff stop / clear      - Stop sniffer / clear queue");
    std::println("  list                    - Show active torrent streams");
    std::println("  peer <hash> <ip>:<port> - Manually inject a peer into a swarm");
    std::println("  quit                    - Shut down the daemon gracefully\n");
    
    std::println("Launch Flags:");
    std::println("  --user-agent <string>   - Spoof a custom User-Agent for direct HTTP links");
    std::println("  --referer <url>         - Spoof a custom Referer for direct HTTP links");
    std::println("  -H, --header <string>   - Pass arbitrary HTTP headers\n");

    while (true) {
        interrupted = false;
        std::cin.clear();
        std::string line;
        
        std::print("daemon> ");
        std::fflush(stdout);
        
        if (!std::getline(std::cin, line)) break;

        if (interrupted) {
            interrupted = false;
            std::println("");
            continue;
        }

        auto start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start, line.find_last_not_of(" \t\r\n") - start + 1);

        if (line == "quit" || line == "q") {
            break;
        } 
        else if (line == "sniff start" || line == "sniff") daemon_.start_sniffer();
        else if (line == "sniff stop") daemon_.stop_sniffer();
        else if (line == "sniff list") daemon_.list_sniffed();
        else if (line == "sniff clear") daemon_.clear_sniffed();
        else if (line.starts_with("sniff play ")) {
            std::string indices_str = line.substr(11);
            std::vector<size_t> targets;
            
            std::string current_num;
            for (char c : indices_str) {
                if (std::isdigit(c)) {
                    current_num += c;
                } else if (!current_num.empty()) {
                    targets.push_back(std::stoull(current_num));
                    current_num.clear();
                }
            }
            if (!current_num.empty()) targets.push_back(std::stoull(current_num));

            if (targets.empty()) std::println("[-] Usage: sniff play <idx1>,<idx2>...");
            else daemon_.play_sniffed(targets);
        }
        else if (line == "list") daemon_.list_streams();
        else if (line.starts_with("stop ")) daemon_.stop_stream(line.substr(5));
        else if (line.starts_with("peer ")) {
            std::string payload = line.substr(5);
            auto space_pos = payload.find(' ');
            if (space_pos != std::string::npos) {
                daemon_.inject_peer(payload.substr(0, space_pos), payload.substr(space_pos + 1));
            } else {
                std::println("[-] Invalid format. Use: peer <hash> <ip>:<port>");
            }
        }
        else if (line.starts_with("add ")) {
            std::string payload = line.substr(4);
            AppConfig temp_config = config_; 
            
            auto extract_flag = [&](const std::string& flag, std::string& out) {
                size_t pos = payload.find(flag);
                if (pos != std::string::npos) {
                    size_t val_start = payload.find_first_not_of(" \t=\"'", pos + flag.length());
                    if (val_start != std::string::npos) {
                        size_t val_end = payload.find_first_of(" \"'", val_start);
                        if (val_end == std::string::npos) val_end = payload.length();
                        out = payload.substr(val_start, val_end - val_start);
                        payload.erase(pos, val_end - pos);
                    }
                }
            };

            extract_flag("--gofile-token", temp_config.gofile_token);
            extract_flag("--user-agent", temp_config.custom_user_agent);
            extract_flag("--referer", temp_config.custom_referer);

            std::string target_url = payload;
            auto ns_start = target_url.find_first_not_of(" \t\r\n\"'");
            if (ns_start != std::string::npos) {
                target_url = target_url.substr(ns_start, target_url.find_last_not_of(" \t\r\n\"'") - ns_start + 1);
            } else target_url = "";

            if (!target_url.empty()) daemon_.add_stream(target_url, temp_config);
        }
        else if (line == "yt") {
            std::println("\n[SYST] Entering yt-dlp mode. Type 'exit' to return.");
            while (true) {
                std::print("yt-dlp> ");
                std::fflush(stdout);
                
                std::string yt_line;
                if (!std::getline(std::cin, yt_line)) break;
                
                auto ys = yt_line.find_first_not_of(" \t\r\n");
                if (ys == std::string::npos) continue;
                yt_line = yt_line.substr(ys, yt_line.find_last_not_of(" \t\r\n") - ys + 1);

                if (yt_line == "exit" || yt_line == "quit") {
                    std::println("[SYST] Returning to core daemon.\n");
                    break;
                }

                if (!yt_line.starts_with("yt-dlp")) {
                    std::println("[-] Unrecognized command. Did you mean to start with 'yt-dlp'?");
                    continue;
                }

                if (yt_line.find("-J") != std::string::npos || yt_line.find("--dump-json") != std::string::npos) {
                    try {
                        auto res = parse_ytdlp_json(yt_line);
                        
                        if (res.is_playlist) {
                            for (size_t i = 0; i < res.entries.size(); ++i) {
                                std::println(" [{}] {}", i, res.entries[i].title);
                            }
                            std::print("\n[?] Enter video number to extract, or 'q' to cancel: ");
                            std::fflush(stdout);
                            std::string choice;
                            std::getline(std::cin, choice);
                            if (choice == "q" || choice == "Q") continue;
                            
                            try {
                                int idx = std::stoi(choice);
                                if (idx >= 0 && idx < res.entries.size()) {
                                    res = parse_ytdlp_json("yt-dlp -J \"" + res.entries[idx].url + "\"");
                                } else continue;
                            } catch(...) { continue; }
                        }

                        if (res.formats.empty()) continue;

                        for (size_t i = 0; i < res.formats.size(); ++i) {
                            const auto& f = res.formats[i];
                            std::println(" [{}] {:<6} | {:<4} | {:<12} (v: {}, a: {}) | {:.2f} MB", 
                                i, f.format_id, f.ext, f.resolution, f.vcodec, f.acodec, f.filesize_mb);
                        }

                        std::print("\n[?] Enter format number to stream (e.g., 25 or 25+6), or 'q' to cancel: ");
                        std::fflush(stdout);
                        std::string choice;
                        std::getline(std::cin, choice);

                        if (choice == "q" || choice == "Q") continue;

                        try {
                            int v_idx = -1, a_idx = -1;
                            size_t plus_pos = choice.find('+');
                            if (plus_pos != std::string::npos) {
                                v_idx = std::stoi(choice.substr(0, plus_pos));
                                a_idx = std::stoi(choice.substr(plus_pos + 1));
                            } else v_idx = std::stoi(choice);

                            if (v_idx >= 0 && v_idx < res.formats.size()) {
                                std::string a_url = (a_idx >= 0 && a_idx < res.formats.size()) ? res.formats[a_idx].url : "";
                                daemon_.add_direct_stream(res.formats[v_idx].url, res.formats[v_idx].headers, a_url, config_);
                            }
                        } catch (...) {}

                    } catch (const std::exception& e) {
                        std::println(stderr, "[-] yt-dlp Error: {}", e.what());
                    }
                } 
                else {
                    system(yt_line.c_str());
                }
            }
        }
        else {
            std::println("Unknown command. Type 'add <link>', 'stop <url>', 'yt', 'sniff start', or 'quit'.");
        }
    }
}
