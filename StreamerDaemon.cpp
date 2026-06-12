#include "StreamerDaemon.h"
#include "ProcessManager.h"
#include "AlertHandler.h"
#include "HttpServer.h"
#include <print>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <boost/asio/ip/tcp.hpp>

StreamerDaemon::StreamerDaemon(const AppConfig& cfg) : config_(cfg) {}

void StreamerDaemon::start() {
    std::error_code ec;
    std::filesystem::create_directories(config_.save_dir, ec);
    if (ec) std::println(stderr, "[-] Warning: Could not create save directory: {}", ec.message());

    lt::settings_pack pack;
    pack.set_bool(lt::settings_pack::enable_dht, true);
    pack.set_bool(lt::settings_pack::enable_upnp, true);
    pack.set_bool(lt::settings_pack::enable_natpmp, true);
    pack.set_bool(lt::settings_pack::enable_lsd, true);
    pack.set_bool(lt::settings_pack::enable_incoming_utp, true);
    pack.set_bool(lt::settings_pack::enable_outgoing_utp, true);
    pack.set_str(lt::settings_pack::dht_bootstrap_nodes, "router.bittorrent.com:6881,router.utorrent.com:6881,dht.transmissionbt.com:6881");
    pack.set_str(lt::settings_pack::listen_interfaces, "0.0.0.0:6881");
    pack.set_int(lt::settings_pack::connections_limit, 500);
    pack.set_int(lt::settings_pack::connection_speed, 100); 
    pack.set_int(lt::settings_pack::torrent_connect_boost, 100);
    pack.set_int(lt::settings_pack::choking_algorithm, lt::settings_pack::fixed_slots_choker);
    pack.set_int(lt::settings_pack::in_enc_policy, lt::settings_pack::pe_enabled);
    pack.set_int(lt::settings_pack::out_enc_policy, lt::settings_pack::pe_enabled);

    lt::alert_category_t alert_mask = lt::alert_category::error | lt::alert_category::status | 
                                      lt::alert_category::storage | lt::alert_category::piece_progress;
                     
    if (config_.debug_mode) {
        std::println("\n[!] DEBUG MODE ENABLED: Writing verbose trace to 'streamer_debug.log'");
        std::ofstream("streamer_debug.log", std::ios::trunc).close();
    }
    pack.set_int(lt::settings_pack::alert_mask, static_cast<int>(static_cast<uint32_t>(alert_mask)));

    manager_.ses.apply_settings(pack);

    alert_thread_ = std::thread(alert_loop, std::ref(manager_), config_.save_dir, config_.debug_mode);
    server_thread_ = std::thread([&]() {
        run_http_server(svr_, manager_, "", config_, interrupted, active_direct_streams_, direct_mtx_);
        svr_.listen("0.0.0.0", config_.port);
    });

    std::println("\n[SYST] Daemon running successfully.");
    std::println("[SYST] Local HTTP Server listening on port {}", config_.port);
}

void StreamerDaemon::shutdown() {
    std::println("\n[SYST] Shutting down daemon... waiting for threads to exit.");
    
    if (sniffer_) sniffer_->stop();
    
    // 1. Gracefully kill Direct HTTP Streams
    {
        std::unique_lock<std::shared_mutex> d_lock(direct_mtx_);
        for (auto& [id, handle] : active_direct_streams_) {
            if (handle.cancel_token) handle.cancel_token->store(true);
            stop_player_by_pid(handle.player_pid);
            
            std::print("[*] Waiting for {} to save cache... ", id);
            std::fflush(stdout);
            int timeout = 0;
            while (handle.finished_token && !handle.finished_token->load() && timeout < 50) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                timeout++;
            }
            std::println("Done!");
        }
    }
    
    // 2. Gracefully kill Torrent Streams
    manager_.ses.pause();
    int outstanding = 0;
    for (auto& [hash, state] : manager_.active_streams) {
        if (state->h.is_valid() && state->h.status().has_metadata) {
            state->resume_data_saved.store(false);
            state->h.save_resume_data(lt::torrent_handle::save_info_dict);
            outstanding++;
        }
    }
    if (outstanding > 0) {
        std::print("[*] Saving {} fastresume files to disk... ", outstanding);
        std::fflush(stdout);
        int timeout = 0;
        bool all_saved = false;
        while (!all_saved && timeout < 50) {
            all_saved = true;
            for (auto& [hash, state] : manager_.active_streams) {
                if (state->h.is_valid() && state->h.status().has_metadata && !state->resume_data_saved.load()) {
                    all_saved = false;
                    break;
                }
            }
            if (!all_saved) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            timeout++;
        }
        std::println("Done!");
    }

    interrupted = true;
    stop_player();
    svr_.stop();
    
    if (server_thread_.joinable()) server_thread_.join();
    if (alert_thread_.joinable()) alert_thread_.join();
}

void StreamerDaemon::add_stream(const std::string& source, const AppConfig& cfg) {
    if (source.starts_with("http://") || source.starts_with("https://")) {
        AppConfig temp_cfg = cfg;
        auto handle = stream_direct_link(temp_cfg, source); 
        std::unique_lock<std::shared_mutex> lock(direct_mtx_);
        active_direct_streams_[handle.stream_id] = handle;
    } else {
        try {
            AppConfig temp_cfg = cfg;
            handle_torrent(manager_, temp_cfg, source);
        } catch (const std::exception& e) {
            std::println(stderr, "[-] Failed to parse torrent/magnet: {}", e.what());
        }
    }
}

void StreamerDaemon::add_direct_stream(const std::string& url, const httplib::Headers& headers, const std::string& audio_url, const AppConfig& cfg) {
    AppConfig temp_cfg = cfg;
    auto handle = stream_direct_link(temp_cfg, url, headers, audio_url);
    std::unique_lock<std::shared_mutex> d_lock(direct_mtx_);
    active_direct_streams_[handle.stream_id] = handle;
}

void StreamerDaemon::stop_stream(const std::string& target) {
    bool found = false;

    {
        std::unique_lock<std::shared_mutex> d_lock(direct_mtx_);
        auto it_dir = std::find_if(active_direct_streams_.begin(), active_direct_streams_.end(),
                               [&target](const auto& pair) { return pair.first.find(target) != std::string::npos; });
        
        if (it_dir != active_direct_streams_.end()) {
            if (it_dir->second.cancel_token) it_dir->second.cancel_token->store(true);
            stop_player_by_pid(it_dir->second.player_pid);
            
            std::print("[*] Saving cache state for direct stream... ");
            std::fflush(stdout);
            int timeout = 0;
            while (it_dir->second.finished_token && !it_dir->second.finished_token->load() && timeout < 50) { 
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                timeout++;
            }
            std::println("Done!");

            std::println("[+] Successfully stopped Direct Stream: {}", it_dir->first);
            active_direct_streams_.erase(it_dir);
            found = true;
        } 
    }

    if (!found) {
        std::unique_lock<std::shared_mutex> lock(manager_.registry_mtx);
        auto it_tor = std::find_if(manager_.active_streams.begin(), manager_.active_streams.end(),
                               [&target](const auto& pair) { return pair.first.find(target) != std::string::npos; });
        
        if (it_tor != manager_.active_streams.end()) {
            auto state = it_tor->second;
            state->shutting_down.store(true);
            state->cv.notify_all(); 
            stop_player_by_pid(state->player_pid); 
            
            if (state->h.is_valid() && state->h.status().has_metadata) {
                std::print("[*] Saving fastresume data... ");
                std::fflush(stdout);
                state->resume_data_saved.store(false);
                state->h.save_resume_data(lt::torrent_handle::save_info_dict);
                
                int timeout = 0;
                while (!state->resume_data_saved.load() && timeout < 30) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    timeout++;
                }
                std::println("Done!");
            }
            
            manager_.ses.remove_torrent(state->h); 
            std::println("[+] Successfully stopped Torrent Stream: {}", it_tor->first);
            manager_.active_streams.erase(it_tor);
            found = true;
        }
    }

    if (!found) std::println("[-] Could not find an active stream matching: {}", target);
}

void StreamerDaemon::list_streams() {
    std::shared_lock<std::shared_mutex> lock(manager_.registry_mtx);
    std::println("\nActive Torrent Streams: {}", manager_.active_streams.size());
    for (const auto& [hash, state] : manager_.active_streams) {
        std::println("  => [{}] {}", hash.substr(0, 8), state->file_path);
        std::println("     URL: http://localhost:{}/stream/{}", config_.port, hash);
    }
    
    std::shared_lock<std::shared_mutex> d_lock(direct_mtx_);
    std::println("\nActive Direct Web Streams: {}", active_direct_streams_.size());
    for (const auto& [id, handle] : active_direct_streams_) {
        std::println("  => {}", id);
    }
}

void StreamerDaemon::inject_peer(const std::string& hash, const std::string& ip_port) {
    auto colon_pos = ip_port.find(':');
    if (colon_pos != std::string::npos) {
        std::string ip = ip_port.substr(0, colon_pos);
        int port = 0;
        try { port = std::stoi(ip_port.substr(colon_pos + 1)); } catch(...) {}

        if (port > 0 && port <= 65535) {
            std::shared_lock<std::shared_mutex> lock(manager_.registry_mtx);
            auto it = std::find_if(manager_.active_streams.begin(), manager_.active_streams.end(),
                                   [&hash](const auto& pair) { return pair.first.starts_with(hash); });
            
            if (it != manager_.active_streams.end()) {
                boost::system::error_code ec;
                auto address = boost::asio::ip::make_address(ip, ec);
                if (!ec) {
                    it->second->h.connect_peer(lt::tcp::endpoint(address, port));
                    std::println("[+] Successfully injected peer {}:{} into swarm for {}", ip, port, hash);
                } else {
                    std::println("[-] Invalid IP address format.");
                }
            } else {
                std::println("[-] Could not find an active stream matching: {}", hash);
            }
        } else {
            std::println("[-] Invalid port number.");
        }
    } else {
        std::println("[-] Invalid format. Use: peer <hash> <ip>:<port>");
    }
}

void StreamerDaemon::start_sniffer() {
    if (!sniffer_) {
        sniffer_ = std::make_shared<NetworkSniffer>("wlan0", 
            [this](const std::string& url, const httplib::Headers& headers) {
                std::lock_guard<std::mutex> lock(sniff_mtx_);
                auto time_t_now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                char time_buf[20];
                std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", std::localtime(&time_t_now));

                intercept_queue_.push_back({url, headers, time_buf});
                
                std::print("\r\033[K"); 
                std::println("[SNIFFER] Captured new media stream! (Total in queue: {})", intercept_queue_.size());
                std::println("          Type 'sniff list' to view or 'sniff play {}' to stream.", intercept_queue_.size() - 1);
                std::print("daemon> ");
                std::fflush(stdout);
            }
        );
        sniffer_->start();
    } else {
        std::println("[*] Sniffer is already active in the background.");
    }
}

void StreamerDaemon::stop_sniffer() {
    if (sniffer_) {
        sniffer_->stop();
        sniffer_.reset();
        std::println("[*] Network sniffer deactivated.");
    } else {
        std::println("[-] Sniffer is not running.");
    }
}

void StreamerDaemon::list_sniffed() {
    std::lock_guard<std::mutex> lock(sniff_mtx_);
    if (intercept_queue_.empty()) {
        std::println("[*] No streams intercepted yet. Make sure 'sniff start' is running.");
    } else {
        std::println("\n=== Intercepted Stream Queue ===");
        for (size_t i = 0; i < intercept_queue_.size(); ++i) {
            std::string display_url = intercept_queue_[i].url;
            if (display_url.length() > 90) display_url = display_url.substr(0, 87) + "...";
            std::println(" [{}] [{}] {}", i, intercept_queue_[i].timestamp, display_url);
        }
        std::println("================================\n");
    }
}

void StreamerDaemon::play_sniffed(const std::vector<size_t>& indices) {
    for (size_t idx : indices) {
        InterceptedStream target;
        {
            std::lock_guard<std::mutex> lock(sniff_mtx_);
            if (idx >= intercept_queue_.size()) {
                std::println("[-] Invalid stream index: {}", idx);
                continue;
            }
            target = intercept_queue_[idx];
        }
        
        std::println("\n[*] Launching intercepted stream [{}]...", idx);
        add_direct_stream(target.url, target.headers, "", config_);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void StreamerDaemon::clear_sniffed() {
    std::lock_guard<std::mutex> lock(sniff_mtx_);
    intercept_queue_.clear();
    std::println("[*] Intercept queue cleared.");
}
