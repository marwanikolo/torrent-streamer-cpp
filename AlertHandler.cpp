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

extern std::atomic<bool> interrupted;

void alert_loop(TorrentManager& manager, const std::string& resume_dir, bool debug_mode) {
    write_debug_log(debug_mode, "[SYST] Multi-Torrent Alert Loop Thread Started");
    
    // Timer for our Telemetry Dashboard
    auto last_telemetry_time = std::chrono::steady_clock::now();

    while (!interrupted.load()) {
        manager.ses.wait_for_alert(lt::milliseconds(200));
        std::vector<lt::alert*> alerts;
        manager.ses.pop_alerts(&alerts);

        for (lt::alert* a : alerts) {
            if (auto* pfa = lt::alert_cast<lt::piece_finished_alert>(a)) {
                std::stringstream ss;
                ss << pfa->handle.info_hashes().v1;
                std::string hash = ss.str();

                auto states = manager.get_streams_by_hash(hash);
                if (!states.empty()) {
                    // SILENT WAKE-UP: We removed the spammy print statement here.
                    // The background HTTP threads are notified instantly without trashing the log.
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

        // --- NEW TELEMETRY TICKER (Executes every 5 seconds) ---
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_telemetry_time).count() >= 5) {
            last_telemetry_time = now;
            
            // Lock the registry to safely read all active streams
            std::shared_lock<std::shared_mutex> lock(manager.registry_mtx);
            for (const auto& [hash, state] : manager.active_streams) {
                if (state->shutting_down.load() || !state->h.is_valid()) continue;
                
                lt::torrent_status ts = state->h.status();
                
                // Convert bytes/second to MB/second
                double dl_rate = ts.download_payload_rate / 1048576.0; 
                double ul_rate = ts.upload_payload_rate / 1048576.0;
                float progress = ts.progress * 100.0f;
                int peers = ts.num_peers;
                int current_playhead = state->latest_piece_requested.load();

                // Build a clean, aligned log line
                std::string log_line = std::format(
                    "[TELE] [{}] DL: {:>5.2f} MB/s | UL: {:>5.2f} MB/s | Peers: {:>3} | Prog: {:>5.1f}% | Playhead: P-{}",
                    hash.substr(0, 8), dl_rate, ul_rate, peers, progress, current_playhead
                );
                
                write_debug_log(debug_mode, log_line);
            }
        }
    }
}
