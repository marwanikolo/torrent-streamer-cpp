#include <iostream>
#include <fstream>
#include <csignal>
#include <filesystem>
#include <print>
#include <thread>
#include <httplib.h>
#include <libtorrent/session.hpp>
#include <libtorrent/alert_types.hpp>

#include "Config.h"
#include "TorrentEngine.h"
#include "AlertHandler.h"
#include "HttpServer.h"
#include "DirectLinkEngine.h"
#include "ProcessManager.h"
#include "YtdlpWrapper.h"

std::atomic<bool> interrupted{false};
void signal_handler(int) { interrupted = true; }

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGCHLD, SIG_IGN);
    
    AppConfig config;
    config.save_dir = "/mnt/NewVolume/Tordown";
    if (config.port <= 0) config.port = 8080; 
    
    std::string initial_source = "";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-p" && i + 1 < argc) config.port = std::stoi(argv[++i]);
        else if (arg == "-d" && i + 1 < argc) config.save_dir = argv[++i];
        else if (arg == "--player" && i + 1 < argc) config.player_path = argv[++i];
        else if (arg == "--debug" || arg == "-v") config.debug_mode = true;
        else initial_source = arg;
    }

    std::error_code ec;
    std::filesystem::create_directories(config.save_dir, ec);
    if (ec) std::println(stderr, "[-] Warning: Could not create save directory: {}", ec.message());

    lt::settings_pack pack;
    pack.set_bool(lt::settings_pack::enable_dht, true);
    pack.set_bool(lt::settings_pack::enable_upnp, true);
    pack.set_bool(lt::settings_pack::enable_natpmp, true);
    pack.set_bool(lt::settings_pack::enable_lsd, true);
    pack.set_bool(lt::settings_pack::enable_incoming_utp, true);
    pack.set_bool(lt::settings_pack::enable_outgoing_utp, true);
    pack.set_str(lt::settings_pack::dht_bootstrap_nodes, 
        "router.bittorrent.com:6881,router.utorrent.com:6881,dht.transmissionbt.com:6881");
    pack.set_int(lt::settings_pack::connections_limit, 200);

    lt::alert_category_t alert_mask = lt::alert_category::error | lt::alert_category::status | 
                                      lt::alert_category::storage | lt::alert_category::piece_progress;
                     
    if (config.debug_mode) {
        alert_mask |= lt::alert_category::torrent_log | lt::alert_category::peer_log;
        std::println("\n[!] DEBUG MODE ENABLED: Writing verbose trace to 'streamer_debug.log'");
        std::ofstream("streamer_debug.log", std::ios::trunc).close();
    }
    pack.set_int(lt::settings_pack::alert_mask, static_cast<int>(static_cast<uint32_t>(alert_mask)));

    TorrentManager manager;
    manager.ses.apply_settings(pack);

    std::thread alert_thread(alert_loop, std::ref(manager), config.save_dir, config.debug_mode);

    httplib::Server svr;
    std::thread server_thread([&]() {
        run_http_server(svr, manager, "", config);
        svr.listen("0.0.0.0", config.port);
    });

    std::println("\n[SYST] Daemon running successfully.");
    std::println("[SYST] Local HTTP Server listening on port {}", config.port);
    std::println("\nCommands:");
    std::println("  add <link>   - Add a new magnet, .torrent, or HTTP link");
    std::println("  yt           - Enter interactive yt-dlp sub-shell");
    std::println("  list         - Show active torrent streams");
    std::println("  quit         - Shut down the daemon gracefully\n");

    if (!initial_source.empty()) {
        auto ns_start = initial_source.find_first_not_of(" \t\r\n\"'");
        if (ns_start != std::string::npos) {
            initial_source = initial_source.substr(ns_start, initial_source.find_last_not_of(" \t\r\n\"'") - ns_start + 1);
        } else {
            initial_source = "";
        }

        if (!initial_source.empty()) {
            if (initial_source.starts_with("http://") || initial_source.starts_with("https://")) {
                stream_direct_link(config, initial_source); 
            } else {
                try {
                    handle_torrent(manager, config, initial_source);
                } catch (const std::exception& e) {
                    std::println(stderr, "[-] Failed to parse torrent/magnet: {}", e.what());
                }
            }
        }
    }

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
        else if (line == "list") {
            std::shared_lock<std::shared_mutex> lock(manager.registry_mtx);
            std::println("Active Streams: {}", manager.active_streams.size());
            for (const auto& [hash, state] : manager.active_streams) {
                std::println("  => [{}] {}", hash.substr(0, 8), state->file_path);
                std::println("     URL: http://localhost:{}/stream/{}", config.port, hash);
            }
        } 
        // --- THE NESTED YT-DLP SUB-SHELL ---
        else if (line == "yt") {
            std::println("\n[SYST] Entering yt-dlp mode.");
            std::println("       Type standard yt-dlp commands (e.g. yt-dlp -F <url>)");
            std::println("       Use '-J' or '--dump-json' to trigger the interactive format menu.");
            std::println("       Type 'exit' to return to daemon>.\n");
            
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

                // If user wants JSON dump, we intercept it and show a menu!
                if (yt_line.find("-J") != std::string::npos || yt_line.find("--dump-json") != std::string::npos) {
                    try {
                        std::println("[*] Fetching and parsing stream formats...");
                        auto formats = parse_ytdlp_json(yt_line);
                        
                        if (formats.empty()) {
                            std::println("[-] No valid streaming formats found.");
                            continue;
                        }

                        std::println("\n======================================================================");
                        std::println("                 AVAILABLE YT-DLP FORMATS");
                        std::println("======================================================================");
                        for (size_t i = 0; i < formats.size(); ++i) {
                            const auto& f = formats[i];
                            // Clean table format: [Index] FormatID | EXT | Resolution (Video, Audio) | Size
                            std::println(" [{}] {:<6} | {:<4} | {:<12} (v: {}, a: {}) | {:.2f} MB", 
                                i, f.format_id, f.ext, f.resolution, f.vcodec, f.acodec, f.filesize_mb);
                        }

                        std::print("\n[?] Enter format number to stream, or 'q' to cancel: ");
                        std::fflush(stdout);
                        std::string choice;
                        std::getline(std::cin, choice);

                        if (choice == "q" || choice == "Q") continue;

                        try {
                            int idx = std::stoi(choice);
                            if (idx >= 0 && idx < formats.size()) {
                                std::println("\n[+] Selected Format [{}] - Proxying stream...", formats[idx].format_id);
                                AppConfig cfg = config;
                                // Pass the extracted URL and CDN Headers directly to your Proxy Engine
                                stream_direct_link(cfg, formats[idx].url, formats[idx].headers);
                            } else {
                                std::println("[-] Invalid selection.");
                            }
                        } catch (...) {
                            std::println("[-] Invalid input.");
                        }

                    } catch (const std::exception& e) {
                        std::println(stderr, "[-] yt-dlp Extraction Error: {}", e.what());
                    }
                } 
                else {
                    // Standard yt-dlp command. Just execute it natively in the shell!
                    int ret = system(yt_line.c_str());
                    if (ret != 0) {
                        std::println(stderr, "[-] yt-dlp exited with code: {}", ret);
                    }
                }
            }
        }
        else if (line.starts_with("add ")) {
            std::string new_source = line.substr(4);
            
            auto ns_start = new_source.find_first_not_of(" \t\r\n\"'");
            if (ns_start != std::string::npos) {
                new_source = new_source.substr(ns_start, new_source.find_last_not_of(" \t\r\n\"'") - ns_start + 1);
            } else {
                new_source = "";
            }

            if (new_source.empty()) continue;

            if (new_source.starts_with("http://") || new_source.starts_with("https://")) {
                stream_direct_link(config, new_source); 
            } else {
                try {
                    handle_torrent(manager, config, new_source);
                } catch (const std::exception& e) {
                    std::println(stderr, "[-] Failed to parse torrent/magnet: {}", e.what());
                    std::println(stderr, "[-] Returning to daemon prompt.");
                }
            }
        } 
        else {
            std::println("Unknown command. Use 'add <link>', 'yt', 'list', or 'quit'.");
        }
    }

    std::println("\n[SYST] Shutting down daemon... waiting for threads to exit.");
    interrupted = true;
    stop_player();
    svr.stop();
    
    if (server_thread.joinable()) server_thread.join();
    if (alert_thread.joinable()) alert_thread.join();

    return 0;
}
