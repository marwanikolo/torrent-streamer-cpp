#include "TorrentEngine.h"
#include "ProcessManager.h"
#include "Utils.h"
#include <iostream>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/write_resume_data.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <print>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>

extern std::atomic<bool> interrupted;

void handle_torrent(TorrentManager& manager, AppConfig& config, std::string source) {
    interrupted = false;
    lt::add_torrent_params atp;
    std::string hash_str;

    if (file_exists(source) && source.find(".torrent") != std::string::npos) {
        atp.ti = std::make_shared<lt::torrent_info>(source);
        hash_str = get_info_hash_string(*atp.ti); 
    } else {
        atp = lt::parse_magnet_uri(source);
        std::stringstream ss;
        ss << atp.info_hashes.get_best();
        hash_str = ss.str();
    }

    atp.save_path = config.save_dir;
    std::string torrent_file_path = config.save_dir + "/" + hash_str + ".torrent";
    std::string resume_file_path = config.save_dir + "/" + hash_str + ".fastresume";

    if (!atp.ti && file_exists(torrent_file_path)) {
        atp.ti = std::make_shared<lt::torrent_info>(torrent_file_path);
    }

    if (file_exists(resume_file_path)) {
        std::ifstream ifs(resume_file_path, std::ios_base::binary);
        ifs.unsetf(std::ios_base::skipws);
        std::vector<char> buf{std::istream_iterator<char>(ifs), std::istream_iterator<char>()};
        lt::error_code ec;
        lt::add_torrent_params resume_params = lt::read_resume_data(buf, ec);
        if (!ec) {
            auto ti_backup = atp.ti; 
            atp = resume_params;
            atp.ti = ti_backup;
            atp.save_path = config.save_dir;
        }
    }

    std::vector<std::string> wss_trackers = {
        "wss://tracker.btorrent.xyz",
        "wss://tracker.openwebtorrent.com",
        "wss://tracker.webtorrent.dev"
    };
    for (const auto& wss : wss_trackers) {
        atp.trackers.push_back(wss);
    }

    lt::torrent_handle h = manager.ses.add_torrent(atp);

    if (!h.status().has_metadata) {
        std::print("[*] Fetching Metadata for new torrent... ");
        std::fflush(stdout);
        while (!h.status().has_metadata) {
            if (interrupted.load()) { manager.ses.remove_torrent(h); return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        std::println("Done!");
        
        std::shared_ptr<const lt::torrent_info> ti_new = h.torrent_file();
        lt::add_torrent_params temp_atp;
        temp_atp.ti = std::make_shared<lt::torrent_info>(*ti_new);
        std::ofstream f(torrent_file_path, std::ios_base::binary);
        std::vector<char> buf;
        lt::bencode(std::back_inserter(buf), lt::write_torrent_file(temp_atp));
        f.write(buf.data(), buf.size());
    }
    
    std::shared_ptr<const lt::torrent_info> ti = h.torrent_file();

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    lt::file_storage const& files = ti->files();
#pragma GCC diagnostic pop

    std::println("\n============================================================");
    std::println("                 AVAILABLE FILES");
    std::println("============================================================");
    for (int i = 0; i < files.num_files(); ++i) {
        std::println(" [{}] {} ({} MB)", i, files.file_path(lt::file_index_t(i)), files.file_size(lt::file_index_t(i)) / (1024 * 1024));
    }

    std::string input;
    std::print("\n[?] Enter file number(s) separated by commas (e.g. 0,2), or 'q' to cancel: ");
    std::fflush(stdout);
    std::getline(std::cin, input);

    if (input == "q" || input == "Q") {
        manager.ses.remove_torrent(h);
        return;
    }

    std::vector<int> selected_indices;
    std::stringstream ss_input(input);
    std::string token;
    while (std::getline(ss_input, token, ',')) {
        try {
            int idx = std::stoi(token);
            if (idx >= 0 && idx < files.num_files()) selected_indices.push_back(idx);
        } catch(...) {}
    }

    if (selected_indices.empty()) {
        std::println("[-] No valid files selected. Canceling torrent.");
        manager.ses.remove_torrent(h);
        return;
    }

    for (int i = 0; i < files.num_files(); ++i) h.file_priority(lt::file_index_t(i), lt::download_priority_t{0});

    std::println("\n[+] Torrent Registered Successfully!");
    std::println("  Name: {}", ti->name());

    for (int idx : selected_indices) {
        h.file_priority(lt::file_index_t(idx), lt::download_priority_t{4});

        auto state = std::make_shared<StreamState>();
        state->h = h;
        state->file_path = config.save_dir + "/" + files.file_path(lt::file_index_t(idx));
        state->file_size = files.file_size(lt::file_index_t(idx));
        state->file_offset = files.file_offset(lt::file_index_t(idx));
        state->piece_length = ti->piece_length();
        state->num_pieces = ti->num_pieces();
        state->first_piece = state->file_offset / state->piece_length;
        state->last_piece = (state->file_offset + state->file_size - 1) / state->piece_length;

        std::string stream_id = hash_str + "_" + std::to_string(idx);
        manager.add_stream(stream_id, state);

        std::string stream_url = "http://localhost:" + std::to_string(config.port) + "/stream/" + stream_id;
        std::string abort_url = "http://localhost:" + std::to_string(config.port) + "/abort/" + stream_id;

        std::println("  => [{}] {} ({} MB)", idx, files.file_path(lt::file_index_t(idx)), files.file_size(lt::file_index_t(idx)) / (1024 * 1024));
        std::println("     URL: {}", stream_url);

        pid_t pid = launch_player(config, stream_url, abort_url);
        state->player_pid = pid;
    }
    std::println("");
}
