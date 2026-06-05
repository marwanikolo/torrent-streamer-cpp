#include <iostream>
#include <fstream>
#include <csignal>
#include <filesystem>
#include <print>
#include <libtorrent/session.hpp>
#include <libtorrent/alert_types.hpp>
#include "Config.h"
#include "TorrentEngine.h"
#include "DirectLinkEngine.h" // <--- NEW: Direct HTTP Engine Integration

// Define the global interrupt flag
std::atomic<bool> interrupted{false};
void signal_handler(int) { interrupted = true; }

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGCHLD, SIG_IGN);
    
    AppConfig config;
    config.save_dir = "/mnt/NewVolume/Tordown";
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
    
    if (ec) {
        std::println(stderr, "[-] Warning: Could not create save directory: {}", ec.message());
    }

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
        
        // Wipe the old log file on fresh startup
        std::ofstream("streamer_debug.log", std::ios::trunc).close();
    }
    
    pack.set_int(lt::settings_pack::alert_mask, static_cast<int>(static_cast<uint32_t>(alert_mask)));
    lt::session ses(pack);

    // Initial source check (Passed via CLI arguments)
    if (!initial_source.empty()) {
        if (initial_source.starts_with("http://") || initial_source.starts_with("https://")) {
            stream_direct_link(config, initial_source);
        } else {
            handle_torrent(ses, config, initial_source);
        }
    }

    // Main interaction loop
    while (true) {
        interrupted = false;
        std::cin.clear();
        std::string new_source;
        
        std::print("\n[>] Source (Magnet, .torrent, or HTTP link) [type 'q' to quit]: ");
        std::fflush(stdout);
        
        std::getline(std::cin, new_source);

        if (interrupted) {
            interrupted = false;
            std::cin.clear();
            std::println("");
            continue;
        }

        auto start = new_source.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        new_source = new_source.substr(start, new_source.find_last_not_of(" \t\r\n") - start + 1);

        if (new_source == "q" || new_source == "Q") break;
        
        // UNIVERSAL ENGINE ROUTING
        if (!new_source.empty()) {
            if (new_source.starts_with("http://") || new_source.starts_with("https://")) {
                stream_direct_link(config, new_source);
            } else {
                handle_torrent(ses, config, new_source);
            }
        }
    }

    return 0;
}
