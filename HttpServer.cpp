#include "HttpServer.h"
#include "WindowManager.h"
#include "Utils.h"
#include "DirectLinkEngine.h" 
#include "ProcessManager.h" 
#include "TorrentEngine.h"
#include "YtdlpWrapper.h"
#include <thread>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <unordered_map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

extern std::atomic<bool> interrupted;
extern std::unordered_map<std::string, DirectStreamHandle> active_direct_streams;

// In-memory cache to hold parsed YouTube formats while the user selects one in the UI
static std::unordered_map<std::string, YtdlpResult> web_yt_cache;

void run_http_server(httplib::Server& svr, TorrentManager& manager, const std::string& hls_playlist, AppConfig& config) {
    bool debug = config.debug_mode;
    
    svr.new_task_queue = [] { return new httplib::ThreadPool(64); };
    
    // --- 1. MOUNT THE WEB UI DIRECTORY ---
    svr.set_mount_point("/", "./public");
    
    // --- 2. ENGINE CONTROL APIs ---
    
    svr.Post("/api/play/torrent", [&manager, &config](const httplib::Request& req, httplib::Response& res) {
        json body = json::parse(req.body);
        std::string url = body.value("url", "");
        // Detach thread so HTTP response returns instantly (metadata fetching can take a few seconds)
        std::thread([&manager, &config, url]() {
            try { handle_torrent(manager, config, url, true); } catch(...) {}
        }).detach();
        res.set_content(R"({"status":"success"})", "application/json");
    });

    svr.Post("/api/play/direct", [&config](const httplib::Request& req, httplib::Response& res) {
        json body = json::parse(req.body);
        std::string url = body.value("url", "");
        auto handle = stream_direct_link(config, url);
        active_direct_streams[handle.stream_id] = handle;
        res.set_content(R"({"status":"success"})", "application/json");
    });

    svr.Post("/api/yt/fetch", [](const httplib::Request& req, httplib::Response& res) {
        json body = json::parse(req.body);
        std::string url = body.value("url", "");
        try {
            auto yt_res = parse_ytdlp_json("yt-dlp -J \"" + url + "\"");
            web_yt_cache[url] = yt_res; 
            
            json out;
            out["is_playlist"] = yt_res.is_playlist;
            if (yt_res.is_playlist) {
                out["entries"] = json::array();
                for (size_t i=0; i < yt_res.entries.size(); ++i) {
                    out["entries"].push_back({{"id", i}, {"title", yt_res.entries[i].title}, {"url", yt_res.entries[i].url}});
                }
            } else {
                out["formats"] = json::array();
                for (size_t i=0; i < yt_res.formats.size(); ++i) {
                    auto& f = yt_res.formats[i];
                    out["formats"].push_back({
                        {"id", i}, {"format_id", f.format_id}, {"ext", f.ext}, 
                        {"resolution", f.resolution}, {"vcodec", f.vcodec}, 
                        {"acodec", f.acodec}, {"size", f.filesize_mb}
                    });
                }
            }
            res.set_content(out.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Post("/api/yt/play", [&config](const httplib::Request& req, httplib::Response& res) {
        json body = json::parse(req.body);
        std::string url = body.value("url", "");
        int v_idx = body.value("v_idx", -1);
        int a_idx = body.value("a_idx", -1);

        if (web_yt_cache.count(url) && v_idx >= 0) {
            auto& yt_res = web_yt_cache[url];
            std::string a_url = (a_idx >= 0) ? yt_res.formats[a_idx].url : "";
            auto handle = stream_direct_link(config, yt_res.formats[v_idx].url, yt_res.formats[v_idx].headers, a_url);
            active_direct_streams[handle.stream_id] = handle;
            res.set_content(R"({"status":"success"})", "application/json");
        } else {
            res.status = 400;
        }
    });

    svr.Post("/api/quit", [&svr](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"success"})", "application/json");
        interrupted = true;
        std::thread([&svr]() { svr.stop(); }).detach();
    });

    // --- 3. STATUS & STOP APIs ---
    
    svr.Get("/api/status", [&manager, &config](const httplib::Request&, httplib::Response& res) {
        json response = { {"torrents", json::array()}, {"direct_streams", json::array()} };
        {
            std::shared_lock<std::shared_mutex> lock(manager.registry_mtx);
            for (const auto& [hash, state] : manager.active_streams) {
                response["torrents"].push_back({
                    {"id", hash},
                    {"name", state->file_path.substr(state->file_path.find_last_of('/') + 1)},
                    {"url", "http://localhost:" + std::to_string(config.port) + "/stream/" + hash}
                });
            }
        }
        for (const auto& [id, handle] : active_direct_streams) {
            response["direct_streams"].push_back({ {"id", id}, {"url", handle.stream_id} });
        }
        res.set_content(response.dump(), "application/json");
    });

    svr.Post("/api/stop", [&manager, debug](const httplib::Request& req, httplib::Response& res) {
        json req_body;
        try { req_body = json::parse(req.body); } catch (...) { res.status = 400; return; }
        std::string target = req_body.value("id", "");
        bool found = false;

        auto it_dir = std::find_if(active_direct_streams.begin(), active_direct_streams.end(),
                               [&target](const auto& pair) { return pair.first.find(target) != std::string::npos; });
        if (it_dir != active_direct_streams.end()) {
            it_dir->second.cancel_token->store(true);
            stop_player_by_pid(it_dir->second.player_pid);
            active_direct_streams.erase(it_dir);
            found = true;
        } else {
            std::unique_lock<std::shared_mutex> lock(manager.registry_mtx);
            auto it_tor = std::find_if(manager.active_streams.begin(), manager.active_streams.end(),
                                   [&target](const auto& pair) { return pair.first.find(target) != std::string::npos; });
            if (it_tor != manager.active_streams.end()) {
                auto state = it_tor->second;
                state->shutting_down.store(true);
                state->cv.notify_all(); 
                stop_player_by_pid(state->player_pid); 
                manager.ses.remove_torrent(state->h); 
                manager.active_streams.erase(it_tor);
                found = true;
            }
        }
        if (found) res.set_content(R"({"status":"success"})", "application/json");
        else res.status = 404;
    });

    // --- 4. PROXY APIs ---
    svr.Get(R"(/abort/([a-fA-F0-9_]+))", [&manager, debug](const httplib::Request& req, httplib::Response& res) {
        std::string hash = req.matches[1];
        if (auto state_ptr = manager.get_stream(hash)) {
            state_ptr->current_request_id++; 
            res.set_content("Aborted", "text/plain");
        } else res.status = 404;
    });

    svr.Get(R"(/stream/([a-fA-F0-9_]+))", [&manager, debug](const httplib::Request& req, httplib::Response& res) {
        std::string hash = req.matches[1];
        auto state_ptr = manager.get_stream(hash); 
        if (!state_ptr) { res.status = 404; return; }

        static std::atomic<int> session_counter{0};
        int my_session_id = ++session_counter;
        int my_epoch = state_ptr->current_request_id.load(); 

        std::string ext = state_ptr->file_path.substr(state_ptr->file_path.find_last_of('.') + 1);
        std::string mime_type = "video/mp4";
        if (ext == "mkv") mime_type = "video/x-matroska";
        else if (ext == "avi") mime_type = "video/x-msvideo";
        else if (ext == "m2ts" || ext == "ts") mime_type = "video/mp2t";
        else if (ext == "iso") mime_type = "application/x-iso9660-image";

        res.set_header("Connection", "close");
        res.set_header("Accept-Ranges", "bytes");
        auto wm = std::make_shared<WindowManager>(*state_ptr);

        res.set_content_provider(state_ptr->file_size, mime_type,
            [state_ptr, wm, my_session_id, my_epoch, debug](size_t offset, size_t length, httplib::DataSink& sink) {
                StreamState& state = *state_ptr; 
                std::int64_t bytes_left = length;
                std::int64_t current_byte = offset; 
                std::ifstream file;
                int empty_reads = 0; 

                while (bytes_left > 0) {
                    if (state.shutting_down.load() || interrupted.load()) return false; 
                    int current_piece = (state.file_offset + current_byte) / state.piece_length;
                    state.latest_piece_requested = current_piece; 
                    
                    int dynamic_window = static_cast<int>((15 * 1024 * 1024) / state.piece_length);
                    wm->update(current_piece, current_piece + std::clamp(dynamic_window, 4, 12)); 

                    while (!state.h.have_piece(lt::piece_index_t(current_piece))) {
                        if (state.shutting_down.load() || interrupted.load()) return false;
                        if (!sink.is_writable() || my_epoch < state.current_request_id.load()) return false; 
                        std::unique_lock<std::mutex> lk(state.mtx);
                        state.cv.wait_for(lk, std::chrono::milliseconds(200));
                    }
                    if (state.shutting_down.load() || interrupted.load()) return false;
                    
                    std::int64_t piece_end_byte = std::min((static_cast<std::int64_t>(current_piece + 1) * state.piece_length) - state.file_offset - 1, state.file_size - 1);
                    std::int64_t chunk_size = std::min({static_cast<std::int64_t>(256 * 1024), static_cast<std::int64_t>(bytes_left), piece_end_byte - current_byte + 1});

                    if (!file.is_open()) file.open(state.file_path, std::ios::binary);
                    else file.clear(); 

                    file.seekg(current_byte);
                    std::vector<char> buffer(chunk_size);
                    file.read(buffer.data(), chunk_size);
                    
                    std::streamsize bytes_read = file.gcount();
                    if (bytes_read == 0) {
                        file.close(); empty_reads++; 
                        if (empty_reads > 100) return false; 
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        continue; 
                    }
                    empty_reads = 0; 
                    if (!sink.write(buffer.data(), bytes_read)) return false;
                    current_byte += bytes_read;
                    bytes_left -= bytes_read;
                }
                return true; 
            }
        );
    });
}
