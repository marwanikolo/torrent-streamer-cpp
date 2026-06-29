#include "AlertHandler.h"
#include "Utils.h"
#include <libtorrent/alert_types.hpp>
#include <libtorrent/write_resume_data.hpp>
#include <libtorrent/torrent_status.hpp>
#include <vector>
#include <fstream>
#include <sstream>
#include <format>
#include <chrono>
#include <shared_mutex>
#include <algorithm>
#include <map>

extern std::atomic<bool> interrupted;

void alert_loop(TorrentManager& manager, const std::string& resume_dir, bool debug_mode) {
    write_debug_log(debug_mode, "[SYST] Multi-Torrent Alert Loop Thread Started");
    
    auto last_telemetry_time = std::chrono::steady_clock::now();
    std::map<std::string, int64_t> previous_download_bytes;

    std::vector<lt::alert*> alerts;
    alerts.reserve(512);
    
    while (!interrupted.load()) {
        manager.ses.wait_for_alert(lt::milliseconds(200));
        
        alerts.clear();
        manager.ses.pop_alerts(&alerts);

        for (lt::alert* a : alerts) {
            if (auto* pfa = lt::alert_cast<lt::piece_finished_alert>(a)) {
                std::stringstream ss;
                ss << pfa->handle.info_hashes().v1;
                std::string hash = ss.str();

                auto states = manager.get_streams_by_hash(hash);
                if (!states.empty()) {
                    for (auto& state_ptr : states) state_ptr->cv.notify_all(); 
                }
            }
            else if (auto* rd = lt::alert_cast<lt::save_resume_data_alert>(a)) {
                std::stringstream ss;
                ss << rd->handle.info_hashes().v1;
                std::string hash = ss.str();

                std::vector<char> buffer = lt::write_resume_data_buf(rd->params);
                std::string resume_file_path = resume_dir + "/" + hash + ".fastresume";
                std::ofstream of(resume_file_path, std::ios_base::binary);
                of.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                
                auto states = manager.get_streams_by_hash(hash);
                for (auto& state_ptr : states) state_ptr->resume_data_saved.store(true);
            }
            else if (auto* rdf = lt::alert_cast<lt::save_resume_data_failed_alert>(a)) {
                std::stringstream ss;
                ss << rdf->handle.info_hashes().v1;
                std::string hash = ss.str();

                auto states = manager.get_streams_by_hash(hash);
                for (auto& state_ptr : states) state_ptr->resume_data_saved.store(true);
            }
        }

        auto now = std::chrono::steady_clock::now();
        double elapsed_seconds = std::chrono::duration_cast<std::chrono::duration<double>>(now - last_telemetry_time).count();

        if (elapsed_seconds >= 5.0) {
            last_telemetry_time = now;
            
            std::shared_lock<std::shared_mutex> lock(manager.registry_mtx);

            // Clean up hashes for torrents that have been removed
            for (auto it = previous_download_bytes.begin(); it != previous_download_bytes.end(); ) {
                if (manager.active_streams.find(it->first) == manager.active_streams.end()) {
                    it = previous_download_bytes.erase(it);
                } else {
                    ++it;
                }
            }
            
            bool is_first = true;
            for (const auto& [hash, state] : manager.active_streams) {
                if (state->shutting_down.load() || !state->h.is_valid()) continue;
                
                if (is_first) {
                    write_debug_log(debug_mode, ""); 
                    is_first = false;
                } else {
                    write_debug_log(debug_mode, "----------------------------------------------------------------------------------");
                }
                
                lt::torrent_status ts = state->h.status();
                
                int64_t current_bytes = ts.total_payload_download;
                double dl_rate = 0.0;
                
                auto it = previous_download_bytes.find(hash);
                if (it != previous_download_bytes.end()) {
                    int64_t previous_bytes = it->second;
                    if (current_bytes > previous_bytes) {
                        dl_rate = ((current_bytes - previous_bytes) / elapsed_seconds) / 1048576.0;
                    }
                } else {
                    dl_rate = ts.download_payload_rate / 1048576.0;
                }
                previous_download_bytes[hash] = current_bytes;

                int peers = ts.num_peers;
                int current_playhead = state->latest_piece_requested.load();

                int have_count = 0;
                int total_file_pieces = state->last_piece - state->first_piece + 1;
                for (int p = state->first_piece; p <= state->last_piece; ++p) {
                    if (state->h.have_piece(lt::piece_index_t(p))) have_count++;
                }
                float progress = (static_cast<float>(have_count) / total_file_pieces) * 100.0f;

                std::vector<int> prioritized_pieces;
                std::vector<lt::download_priority_t> priorities = state->h.get_piece_priorities();
                for (int i = 0; i < priorities.size(); ++i) {
                    if (static_cast<uint8_t>(priorities[i]) > 0 && !state->h.have_piece(lt::piece_index_t(i))) {
                        prioritized_pieces.push_back(i);
                    }
                }
                
                size_t win_limit = 12;
                size_t actual_win_limit = std::min(win_limit, prioritized_pieces.size());
                if (actual_win_limit > 0) {
                    std::partial_sort(prioritized_pieces.begin(), prioritized_pieces.begin() + actual_win_limit, prioritized_pieces.end(), [&priorities](int a, int b) {
                        uint8_t prio_a = static_cast<uint8_t>(priorities[a]);
                        uint8_t prio_b = static_cast<uint8_t>(priorities[b]);
                        if (prio_a != prio_b) return prio_a > prio_b; 
                        return a < b; 
                    });
                }
                
                std::string win_str;
                win_str.reserve(128);
                win_str = "[";
                for (size_t i = 0; i < actual_win_limit; ++i) {
                    int p_idx = prioritized_pieces[i];
                    uint8_t p_val = static_cast<uint8_t>(priorities[p_idx]);
                    win_str += std::format("{}(p{})", p_idx, p_val);
                    if (i < actual_win_limit - 1) win_str += ", ";
                }
                if (prioritized_pieces.size() > win_limit) win_str += std::format(" ... +{} more", prioritized_pieces.size() - win_limit);
                win_str += "]";
                if (prioritized_pieces.empty()) win_str = "[None]";

                auto queue = state->h.get_download_queue();
                std::vector<int> inflight_pieces;
                for (const auto& q : queue) {
                    inflight_pieces.push_back(static_cast<int>(q.piece_index));
                }
                
                size_t flight_limit = 8;
                size_t actual_flight_limit = std::min(flight_limit, inflight_pieces.size());
                if (actual_flight_limit > 0) {
                    std::partial_sort(inflight_pieces.begin(), inflight_pieces.begin() + actual_flight_limit, inflight_pieces.end(), [&priorities](int a, int b) {
                        uint8_t prio_a = static_cast<uint8_t>(priorities[a]);
                        uint8_t prio_b = static_cast<uint8_t>(priorities[b]);
                        if (prio_a != prio_b) return prio_a > prio_b;
                        return a < b;
                    });
                }
                
                std::string flight_str;
                flight_str.reserve(128);
                flight_str = "[";
                for (size_t i = 0; i < actual_flight_limit; ++i) {
                    int p_idx = inflight_pieces[i];
                    uint8_t p_val = static_cast<uint8_t>(priorities[p_idx]);
                    flight_str += std::format("{}(p{})", p_idx, p_val);
                    if (i < actual_flight_limit - 1) flight_str += ", ";
                }
                if (inflight_pieces.size() > flight_limit) flight_str += std::format(" ... +{} more", inflight_pieces.size() - flight_limit);
                flight_str += "]";
                if (inflight_pieces.empty()) flight_str = "[None]";

                std::string log_line = std::format(
                    "[TELE] [{}] DL: {:>5.2f} MB/s | Peers: {:>3} | Prog: {:>5.1f}% | Head: P-{:<5} | Win: {:<40} | Flight: {}",
                    hash.substr(0, 8), dl_rate, peers, progress, current_playhead, win_str, flight_str
                );
                
                write_debug_log(debug_mode, log_line);
            }
        }
    }
}