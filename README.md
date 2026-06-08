# C++ BitTorrent Streaming Daemon

A high-performance, concurrent BitTorrent and Direct HTTP streaming daemon built in C++. This application allows you to instantly stream video/audio files directly from magnet links, `.torrent` files, Direct HTTP web links, or YouTube/DASH playlists.

Recently rewritten into a true multi-threaded background daemon, it features an interactive CLI, a REST API Web UI, dynamic proxy routing, sequential downloading, smart bandwidth prioritization, and zero-latency Lua seeking.

## ✨ Features

* **Concurrent Multi-Streaming:** Stream multiple torrents, season packs, and direct HTTP links simultaneously. The daemon dynamically allocates ports and manages isolated background threads for every active stream.
* **Stateless Web UI & REST API:** Features a fully integrated HTTP server hosting a stateless Web UI. Add torrents, monitor active streams, and gracefully terminate processes directly from your browser via the `/api` endpoints with `auto_play_largest` support.
* **Interactive Season Pack Support:** Prompts for interactive file selection on multi-file torrents. Select multiple episodes (e.g., `0,1,2`) to instantly spawn multiple synchronized media player windows at once.
* **YouTube & DASH Integration:** Features a built-in `yt-dlp` interactive sub-shell. Parses JSON manifests to extract multi-format playlists, allowing you to proxy, cache, and actively merge split video/audio tracks in real-time.
* **Headless Daemon CLI:** Features a clean, non-blocking interactive prompt (`daemon>`). The primary terminal remains clean, while background server threads process commands instantly.
* **Lightning-Fast Sequential Streaming:** Built on `libtorrent-rasterbar`, forcing sequential piece downloading and aggressive metadata pre-fetching (Head & Tail pieces) for instant startup.
* **Thread-Safe Reference Counting:** Safely manages concurrent HTTP connections from aggressive media players (like MPV's internal background caching) without corrupting piece priorities or starving the active stream.
* **Zero-Latency Active Kill Seeking:** Uses dynamically injected, unique Lua scripts (MPV event hooks and VLC background interfaces) to provide out-of-band signaling. This proactively kills obsolete HTTP worker threads the millisecond you seek, preventing BitTorrent "phantom priority deadlocks."
* **Universal Direct HTTP Engine:** Paste any standard video URL to proxy, cache, and stream it through the C++ engine. Features dynamic port binding to prevent collision with torrent traffic.
* **Native ISO & Blu-Ray VFS Mounting:** Automatically detects `.iso` files, routes playback to VLC, and disables BD-J (Java) menus to seamlessly stream 50GB+ physical disk images over BitTorrent using VLC's Virtual File System.
* **Graceful Shutdown & Fast Start:** Traps `SIGINT` (Ctrl+C) and utilizes a multi-kill TCP deadlock unblocker to instantly terminate all spawned media players and serialize `.fastresume` data to disk for instant re-launches.

## 🧠 Architecture & Under the Hood

This engine relies on a strictly decoupled, concurrent architecture that separates the chaotic HTTP frontend from the stable BitTorrent backend.

### 1. The Central Demultiplexer Registry
The daemon runs a global `TorrentManager` backed by a `std::shared_mutex` read-write lock. When Libtorrent alerts the system that a piece has finished downloading, the background router inspects the `info_hash`, checks the central registry, and wakes up *only* the specific HTTP threads waiting for that exact torrent file. 

### 2. The Sliding Window Manager & Master Session Logic
Video players (`mpv`/`vlc`) send chaotic, rapidly shifting HTTP byte-range requests, and often keep old sockets alive in the background to pre-cache data. If these were passed directly to the swarm, the BitTorrent engine would choke. 
* **Thread-Safe Reference Counting:** The Window Manager tracks piece requests across all concurrent player connections. Pieces are only deprioritized when their global reference count hits zero.
* **Master Session Delegation:** To prevent background caching threads from stealing bandwidth, the daemon dynamically tracks the "Master Session" (the most recent HTTP request). The Master dictates the true playhead, granting it an aggressive `max(2, 7 - distance)` priority cascade and strict `0ms` deadlines, while instantly demoting all background threads to a passive priority.

## 🛠️ Dependencies

* **C++26** (Requires a modern compiler: GCC 14+ or Clang 18+)
* **CMake** (Build system 3.16+)
* **libtorrent-rasterbar** (v2.0+)
* **cpp-httplib** (Header-only HTTP server)
* **nlohmann/json** (For Web API & yt-dlp parsing)
* **curl** (Required for out-of-band Lua seek signaling)
* **yt-dlp** (For YouTube/DASH extraction)
* **mpv** and/or **VLC** (For playback)

## 🚀 Building from Source

```bash
# Clone the repository
git clone [https://github.com/marwanikolo/torrent-streamer-cpp.git](https://github.com/marwanikolo/torrent-streamer-cpp.git)
cd torrent-streamer-cpp

# Create build directory
mkdir build && cd build

# Configure and compile
cmake ..
make -j$(nproc)
```

## 🎮 Usage & Advanced Configuration

Launch the daemon via the terminal. You can pass an initial link via CLI arguments, or simply start the daemon to enter the interactive REPL.

```bash
./build/streamer 
```

### The Interactive Daemon Prompt (`daemon>`)
Once the background servers initialize, you will be dropped into the interactive command loop.

* `add <link>`: Paste a magnet link, local `.torrent` path, or HTTP link. The daemon will automatically parse it, launch the background proxies, and pop open your video player.
* `yt`: Enters the interactive `yt-dlp` sub-shell. Use `-J` or `--dump-json` to extract format lists and select specific video/audio streams to proxy.
* `list`: Displays a real-time list of all active BitTorrent and Direct Web streams, showing their file paths and local HTTP proxy URLs.
* `stop <hash/url>`: Gracefully terminates a specific active stream, kills the associated media player window, and saves `.fastresume` data.
* `quit`: Safely kills all active MPV/VLC windows, terminates all network connections, saves global fastresume data, and shuts down the daemon.

### The Telemetry Dashboard
Because the daemon UI is kept perfectly clean, you can monitor the real-time background network traffic by opening a second terminal window. The daemon outputs a clean, 5-second telemetry heartbeat showing download/upload speeds, active peers, absolute physical file progress, and the exact sliding window playhead position (sorted natively by priority).

```bash
tail -f streamer_debug.log
```

### Command Line Arguments
* `-p, --port <port>`: Change the primary local HTTP Torrent server port (Default: `8080`). Direct HTTP links will dynamically allocate ports sequentially above this number.
* `-d, --dir <path>`: Change the download and cache directory (Default: `/mnt/NewVolume/Tordown`).
* `--player <path>`: Define a custom path or executable for your media player (e.g., `mpv` or `vlc`). *Note: The engine will dynamically override this to `vlc` if an `.iso` file is selected to ensure VFS support.*
* `-v, --debug`: Enables verbose mode. Writes the telemetry dashboard, raw HTTP socket traces, and proxy warnings to `streamer_debug.log`.
