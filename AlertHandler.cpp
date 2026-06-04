#include "AlertHandler.h"
#include "Utils.h"
#include <libtorrent/alert_types.hpp>
#include <libtorrent/write_resume_data.hpp>
#include <vector>
#include <fstream>
#include <format>
#include <chrono>

void alert_loop(lt::session& ses, StreamState* state, const std::string& resume_path, bool debug_mode) {
    write_debug_log(debug_mode, "[SYST] Alert Loop Thread Started");
    
    while (state && !state->shutting_down.load()) {
        ses.wait_for_alert(lt::milliseconds(200));
        std::vector<lt::alert*> alerts;
        ses.pop_alerts(&alerts);

        for (lt::alert* a : alerts) {
            if (auto* pfa = lt::alert_cast<lt::piece_finished_alert>(a)) {
                write_debug_log(debug_mode, std::format("[SYST] Finished downloading piece: {}", static_cast<int>(pfa->piece_index)));
                state->cv.notify_all(); 
            }
        }
    }

    auto shutdown_start = std::chrono::steady_clock::now();
    while (state && !state->resume_data_saved.load()) {
        if (std::chrono::steady_clock::now() - shutdown_start > std::chrono::seconds(2)) {
            write_debug_log(debug_mode, "[SYST] Alert Loop: Timeout waiting for resume data. Exiting.");
            break; 
        }

        ses.wait_for_alert(lt::milliseconds(200));
        std::vector<lt::alert*> alerts;
        ses.pop_alerts(&alerts);

        for (lt::alert* a : alerts) {
            if (auto* rd = lt::alert_cast<lt::save_resume_data_alert>(a)) {
                std::vector<char> buffer = lt::write_resume_data_buf(rd->params);
                std::ofstream of(resume_path, std::ios_base::binary);
                of.write(buffer.data(), buffer.size());
                state->resume_data_saved = true; 
            }
            else if (lt::alert_cast<lt::save_resume_data_failed_alert>(a)) {
                state->resume_data_saved = true; 
            }
        }
    }
}
