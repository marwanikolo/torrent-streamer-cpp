#include "TorrentEngine.h"
#include "StreamEngine.h"
#include "Utils.h"
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/write_resume_data.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/settings_pack.hpp>
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>

extern std::atomic<bool> interrupted;

void handle_torrent(lt::session& ses, AppConfig& config, std::string source) {
    interrupted = false;
    std::cin.clear();

    lt::settings_pack pack;
    pack.set_str(lt::settings_pack::listen_interfaces, "0.0.0.0:6881");
    
    pack.set_int(lt::settings_pack::alert_mask, static_cast<int>(static_cast<uint32_t>(
        lt::alert_category::error | 
        lt::alert_category::status | 
        lt::alert_category::storage | 
        lt::alert_category::file_progress
    )));
        
    ses.apply_settings(pack);

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

    lt::torrent_handle h = ses.add_torrent(atp);

    if (!h.status().has_metadata) {
        std::cout << "\n[*] Waiting for Metadata...\n";
        while (!h.status().has_metadata) {
            if (interrupted.load()) { ses.remove_torrent(h); interrupted = false; return; }
            lt::torrent_status st = h.status();
            std::cout << "\r[>] DHT/LSD Peers: " << st.num_peers << " | Searching...   " << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        
        std::shared_ptr<const lt::torrent_info> ti_new = h.torrent_file();
        
        // FIX: libtorrent 2.x deprecation fix for create_torrent
        lt::add_torrent_params temp_atp;
        temp_atp.ti = std::make_shared<lt::torrent_info>(*ti_new);
        
        std::ofstream f(torrent_file_path, std::ios_base::binary);
        std::vector<char> buf;
        lt::bencode(std::back_inserter(buf), lt::write_torrent_file(temp_atp));
        f.write(buf.data(), buf.size());
    }
    
    std::shared_ptr<const lt::torrent_info> ti = h.torrent_file();

    while (true) {
        interrupted = false; 
        std::cin.clear();
        std::cout << "\n\n============================================================\n";
        std::cout << "                 AVAILABLE FILES\n";
        std::cout << "============================================================\n";
        
        // FIX: libtorrent 2.x deprecation fix for ti->files()
        lt::file_storage const& files = ti->orig_files();
        for (int i = 0; i < files.num_files(); ++i) {
            std::cout << " [" << i << "] " << files.file_path(lt::file_index_t(i)) 
                      << " (" << files.file_size(lt::file_index_t(i)) / (1024 * 1024) << " MB)\n";
        }

        std::string input;
        std::cout << "\n[?] Enter file number, 'b' to go back, 'q' to quit: ";
        
        std::getline(std::cin, input);

        if (interrupted.load()) {
            interrupted = false;
            std::cin.clear();
            input = "b"; 
        }

        auto start = input.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue; 
        input = input.substr(start, input.find_last_not_of(" \t\r\n") - start + 1);

        if (input == "b" || input == "B") {
            h.save_resume_data();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            ses.remove_torrent(h);
            return;
        }
        if (input == "q" || input == "Q") {
            h.save_resume_data();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            exit(0);
        }

        int choice = -1;
        try { choice = std::stoi(input); } catch(...) {}

        if (choice < 0 || choice >= files.num_files()) {
            std::cerr << "[-] Invalid selection. Try again.\n";
            continue;
        }

        stream_file(ses, config, h, ti, choice, resume_file_path);
    }
}
