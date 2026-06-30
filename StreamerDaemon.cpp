#include "StreamerDaemon.h"
#include "ProcessManager.h"
#include "AlertHandler.h"
#include "HttpServer.h"
#include <print>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <algorithm>
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
    // 1. Web Links & Local HLS Playlists go to the Proxy Engine
    if (source.starts_with("http://") || source.starts_with("https://") || source.ends_with(".m3u8")) {
        AppConfig temp_cfg = cfg;
        auto handle = stream_direct_link(temp_cfg, source); 
        std::unique_lock<std::shared_mutex> lock(direct_mtx_);
        active_direct_streams_[handle.stream_id] = handle;
    } 
    // 2. Torrents and Magnets go to libtorrent
    else if (source.find(".torrent") != std::string::npos || source.starts_with("magnet:?")) {
        try {
            AppConfig temp_cfg = cfg;
            handle_torrent(manager_, temp_cfg, source);
        } catch (const std::exception& e) {
            std::println(stderr, "[-] Failed to parse torrent/magnet: {}", e.what());
        }
    }
    // 3. Fallback: It is a standard local file (.mp4, .mkv, etc.)
    else if (std::filesystem::exists(source)) {
        std::println("[*] Detected local media file. Launching player natively...");
        pid_t pid = launch_player(cfg, source, "", "");
        
        DirectStreamHandle handle;
        handle.stream_id = "local_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        handle.player_pid = pid;
        handle.cancel_token = std::make_shared<std::atomic<bool>>(false);
        handle.finished_token = std::make_shared<std::atomic<bool>>(true);
        
        std::unique_lock<std::shared_mutex> lock(direct_mtx_);
        active_direct_streams_[handle.stream_id] = handle;
    }
    else {
        std::println(stderr, "[-] Invalid input: Not a valid HTTP link, Magnet URI, or existing local file.");
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

static std::string guess_media_type(const std::string& url) {
    std::string lower_url = url;
    std::transform(lower_url.begin(), lower_url.end(), lower_url.begin(), ::tolower);

    if (lower_url.find("twimg.com") != std::string::npos) {
        if (lower_url.find(".m3u8") != std::string::npos) return "TW/X PLAYLIST";
        if (lower_url.find("/vid/") != std::string::npos) return "TW/X VIDEO";
        if (lower_url.find("/aud/") != std::string::npos) return "TW/X AUDIO";
        return "TW/X MEDIA";
    }
    if (lower_url.find("instagram") != std::string::npos || lower_url.find("cdninstagram") != std::string::npos) return "IG MEDIA";
    if (lower_url.find("tiktok.com") != std::string::npos) return "TIKTOK";
    if (lower_url.find("redgifs.com") != std::string::npos) {
        if (lower_url.find("/hd.") != std::string::npos) return "REDGIFS HD";
        if (lower_url.find("/sd.") != std::string::npos) return "REDGIFS SD";
        return "REDGIFS";
    }
    if (lower_url.find("gofile.io") != std::string::npos) return "GOFILE";
    if (lower_url.find("k2s.cc") != std::string::npos || lower_url.find("keep2share") != std::string::npos) return "KEEP2SHARE";
    if (lower_url.find("filestore.app") != std::string::npos || lower_url.find("tezfiles") != std::string::npos) return "TEZFILES";
    if (lower_url.find("pixeldrain.com") != std::string::npos) return "PIXELDRAIN";
    
    if (lower_url.find(".m3u8") != std::string::npos) return "HLS PLAYLIST";
    if (lower_url.find(".mp4") != std::string::npos) return "MP4 VIDEO";
    if (lower_url.find(".webm") != std::string::npos) return "WEBM VIDEO";
    
    return "WEB MEDIA";
}

void StreamerDaemon::start_sniffer() {
    if (!sniffer_) {
        sniffer_ = std::make_shared<NetworkSniffer>("wlan0", 
            [this](const std::string& url, const httplib::Headers& headers) {
                
                std::string tag = guess_media_type(url);
                bool is_playlist = (tag.find("PLAYLIST") != std::string::npos);

                auto time_t_now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                char time_buf[20];
                std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", std::localtime(&time_t_now));

                {
                    std::lock_guard<std::mutex> lock(sniff_mtx_);
                    std::string init_type = is_playlist ? "m3u8/hls" : "";
                    intercept_queue_.push_back({url, headers, time_buf, -1, tag, init_type, ""});
                }
                
                std::print("\r\033[K"); 
                std::println("[SNIFFER] Captured new media stream! (Total in queue: {})", intercept_queue_.size());
                std::println("          Type 'sniff list' to view or 'sniff play {}' to stream.", intercept_queue_.size() - 1);
                std::print("daemon> ");
                std::fflush(stdout);

                // =========================================================================
                // THE BACKGROUND PROBE (Size, Content-Type, AND ffprobe Resolution)
                // =========================================================================
                if (!is_playlist) {
                    std::thread([this, url, headers]() {
                        size_t p_pos = url.find("://");
                        size_t h_start = (p_pos != std::string::npos) ? p_pos + 3 : 0;
                        size_t path_start = url.find('/', h_start);
                        std::string host = (path_start == std::string::npos) ? url : url.substr(0, path_start);
                        std::string path = (path_start == std::string::npos) ? "/" : url.substr(path_start);

                        httplib::Client cli(host);
                        cli.enable_server_certificate_verification(false);
                        cli.set_connection_timeout(3);
                        cli.set_read_timeout(3);
                        
                        httplib::Headers safe_headers;
                        std::string ffprobe_headers = ""; // Build the header string for ffprobe
                        
                        for (const auto& [k, v] : headers) {
                            std::string kl = k; std::transform(kl.begin(), kl.end(), kl.begin(), ::tolower);
                            if (kl != "host" && kl != "range" && kl != "connection" && kl != "accept-encoding") {
                                std::string cv = v, ck = k;
                                cv.erase(std::remove(cv.begin(), cv.end(), '\r'), cv.end());
                                cv.erase(std::remove(cv.begin(), cv.end(), '\n'), cv.end());
                                safe_headers.emplace(ck, cv);
                                ffprobe_headers += ck + ": " + cv + "\r\n";
                            }
                        }

                        // 1. Lightweight HEAD request for Size and Type
                        auto res = cli.Head(path.c_str(), safe_headers);
                        std::int64_t size = -1;
                        std::string c_type = "";
                        
                        if (res && res->status < 400) {
                            if (res->has_header("Content-Length")) size = std::stoll(res->get_header_value("Content-Length"));
                            if (res->has_header("Content-Type")) c_type = res->get_header_value("Content-Type");
                        } else {
                            safe_headers.emplace("Range", "bytes=0-0");
                            auto get_res = cli.Get(path.c_str(), safe_headers);
                            if (get_res && get_res->status == 206) {
                                if (get_res->has_header("Content-Range")) {
                                    std::string cr = get_res->get_header_value("Content-Range");
                                    size_t slash = cr.find('/');
                                    if (slash != std::string::npos) size = std::stoll(cr.substr(slash + 1));
                                }
                                if (get_res->has_header("Content-Type")) c_type = get_res->get_header_value("Content-Type");
                            }
                        }

                        // 2. Heavy ffprobe execution (Only if it looks like actual media)
                        std::string resolution = "";
                        if (size > 100000 || c_type.find("video") != std::string::npos) { 
                            auto escape_sh = [](std::string s) {
                                size_t pos = 0;
                                while ((pos = s.find('\'', pos)) != std::string::npos) {
                                    s.replace(pos, 1, "'\\''");
                                    pos += 4;
                                }
                                return s;
                            };

                            // The magic ffprobe CSV formatter
                            std::string cmd = std::format("ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=s=x:p=0 -headers '{}' '{}' 2>/dev/null", escape_sh(ffprobe_headers), escape_sh(url));
                            
                            FILE* pipe = popen(cmd.c_str(), "r");
                            if (pipe) {
                                char buffer[128];
                                if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                                    resolution = buffer;
                                    resolution.erase(std::remove(resolution.begin(), resolution.end(), '\n'), resolution.end());
                                    resolution.erase(std::remove(resolution.begin(), resolution.end(), '\r'), resolution.end());
                                }
                                pclose(pipe);
                            }
                        }

                        // 3. Update the UI Struct safely
                        if (size > 0 || !c_type.empty() || !resolution.empty()) {
                            std::lock_guard<std::mutex> lock(sniff_mtx_);
                            for (auto& s : intercept_queue_) {
                                if (s.url == url) {
                                    if (size > 0) s.size_bytes = size;
                                    if (!c_type.empty()) s.content_type = c_type;
                                    if (!resolution.empty()) s.resolution = resolution;
                                    break;
                                }
                            }
                        }
                    }).detach();
                }
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
    // Legacy fallback, we use InteractiveShell's dashboard now
    std::lock_guard<std::mutex> lock(sniff_mtx_);
    if (intercept_queue_.empty()) {
        std::println("[*] No streams intercepted yet. Make sure 'sniff start' is running.");
    } else {
        std::println("\n=== Intercepted Stream Queue ===");
        for (size_t i = 0; i < intercept_queue_.size(); ++i) {
            std::string display_url = intercept_queue_[i].url;
            if (display_url.length() > 60) display_url = display_url.substr(0, 57) + "...";
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

std::vector<InterceptedStream> StreamerDaemon::get_intercept_queue() {
    std::lock_guard<std::mutex> lock(sniff_mtx_);
    return intercept_queue_;
}
