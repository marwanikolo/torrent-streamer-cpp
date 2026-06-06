#include "AlertHandler.h"
#include "Utils.h"
#include <libtorrent/alert_types.hpp>
#include <libtorrent/write_resume_data.hpp>
#include <vector>
#include <fstream>
#include <sstream>
#include <format>
#include <chrono>

extern std::atomic<bool> interrupted;

void alert_loop(TorrentManager& manager, const std::string& resume_dir, bool debug_mode) {
    write_debug_log(debug_mode, "[SYST] Multi-Torrent Alert Loop Thread Started");
    
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
                    write_debug_log(debug_mode, std::format("[SYST] [{}] Finished piece: {}", hash.substr(0, 8), static_cast<int>(pfa->piece_index)));
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
    }
}
