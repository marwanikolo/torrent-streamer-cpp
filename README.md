# 🎬 C++ BitTorrent Streaming Daemon

![C++26](https://img.shields.io/badge/C%2B%2B-26-blue.svg)
![CMake](https://img.shields.io/badge/CMake-3.16+-green.svg)
![libtorrent](https://img.shields.io/badge/libtorrent-2.0+-orange.svg)
![Platform](https://img.shields.io/badge/Platform-Linux-lightgrey.svg)

A high-performance, concurrent BitTorrent and Direct HTTP streaming daemon built in modern C++26. This application allows you to instantly stream video/audio files directly from magnet links, `.torrent` files, Direct HTTP web links, or YouTube/DASH playlists.

Designed as a true multi-threaded background daemon, it features an interactive CLI, a REST API Web UI, dynamic proxy routing, sequential downloading, smart bandwidth prioritization, and zero-latency Lua seeking.

## ✨ Killer Features

* **Zero-Copy Memory Mapping (`mmap`):** Bypasses expensive standard C++ file stream (`ifstream`) syscalls. The HTTP frontend serves chunks directly from the Linux Kernel's page cache, utilizing `madvise(MADV_SEQUENTIAL)` to aggressively free RAM during massive (50GB+) ISO streaming.
* **Concurrent Multi-Streaming:** Stream multiple torrents, season packs, and direct HTTP links simultaneously. The daemon dynamically allocates ports and manages isolated background threads for every active stream.
* **Stateless Web UI & REST API:** Features a fully integrated HTTP server hosting a stateless Web UI. Add torrents, monitor active streams, and gracefully terminate processes directly from your browser via the `/api` endpoints.
* **Interactive Season Pack Support:** Prompts for interactive file selection on multi-file torrents. Select multiple episodes (e.g., `0,1,2`) to instantly spawn multiple synchronized media player windows at once.
* **YouTube & DASH Integration:** Features a built-in `yt-dlp` interactive sub-shell. Parses JSON manifests to extract multi-format playlists, allowing you to proxy, cache, and actively merge split video/audio tracks in real-time.
* **Native ISO & Blu-Ray Parsing:** Automatically detects physical disk images (`.iso`), parses their internal binary `.clpi` (Clip Information) files, maps the physical `SPN` byte offsets, and generates dynamic Apple HLS (`.m3u8`) playlists on the fly. Stream 50GB+ Blu-rays sequentially over BitTorrent without extracting or transcoding.
* **Terminal File Manager Integration:** Includes a native bash wrapper script (`scripts/streamer-wrapper.sh`) that allows seamless "Single-Instance" remote control from terminal file managers like **Yazi**, **Ranger**, or **LF**.

## 🧠 Architecture & Under the Hood

This engine relies on a strictly decoupled, concurrent architecture that separates the chaotic HTTP frontend from the stable BitTorrent backend.

### 1. Anti-Swarm-Shock (In-Flight Shield)
Rapidly seeking across a video file normally causes standard BitTorrent clients to spam `CANCEL` messages, resulting in massive peer drops. This engine uses a "Soft-Cancel" In-Flight Shield to gracefully demote active pieces to a Priority 1 Scavenger Tier, keeping TCP sockets alive and peers happy while instantly pivoting swarm bandwidth to the new playhead.

### 2. Thread-Safe Reference Counting & Master Sessions
Video players (`mpv`/`vlc`) send chaotic, rapidly shifting HTTP byte-range requests and keep old sockets alive to pre-cache data.
* **Window Manager:** Tracks piece requests across all concurrent player connections. Pieces are only deprioritized when their global reference count hits zero.
* **Master Session Delegation:** Dynamically tracks the "Master Session" (the most recent HTTP request). The Master dictates the true playhead, granting it an aggressive `max(2, 7 - distance)` priority cascade and strict `0ms` deadlines, while instantly demoting all background threads to a passive Priority 1.

### 3. Zero-Latency Active Kill Seeking
Uses dynamically injected, unique Lua scripts (MPV event hooks and VLC background interfaces) to provide out-of-band signaling. This proactively kills obsolete HTTP worker threads the millisecond you seek, preventing BitTorrent "phantom priority deadlocks."

### 4. RAII Thread Lifecycle (C++20 `std::jthread`)
The REST API and proxy servers utilize modern `std::jthread` and `std::shared_mutex` dependency injection. When the daemon receives a `SIGINT` (Ctrl+C), it seamlessly tracks and waits for all active HTTP workers to flush their I/O before safely serializing `.fastresume` data and terminating.

## 🛠️ Dependencies

* **C++26 Compiler** (GCC 14+ or Clang 18+)
* **CMake** (3.16+)
* **libtorrent-rasterbar** (v2.0+)
* **cpp-httplib** (Header-only HTTP server)
* **nlohmann/json** (For Web API & yt-dlp parsing)
* **curl** (Required for out-of-band Lua seek signaling)
* **yt-dlp** (For YouTube/DASH extraction)
* **mpv** and/or **VLC** (For playback)

## 🚀 Building from Source

```bash
# Clone the repository
git clone https://github.com/marwanikolo/torrent-streamer-cpp.git
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

* `add <link>`: Paste a magnet link, local `.torrent` path, or HTTP link.
* `yt`: Enters the interactive `yt-dlp` sub-shell. Use `-J` or `--dump-json` to extract format lists and select specific video/audio streams to proxy.
* `list`: Displays a real-time list of all active BitTorrent and Direct Web streams.
* `stop <hash/url>`: Gracefully terminates a specific active stream and saves `.fastresume` data.
* `peer <hash> <ip>:<port>`: Manually injects a specific peer (e.g., from a private tracker) directly into the libtorrent swarm via a raw TCP/µTP handshake.
* `quit`: Safely kills all active MPV/VLC windows, terminates all network connections, saves global fastresume data, and shuts down the daemon.

### Yazi / Terminal File Manager Integration
You can use the provided `scripts/streamer-wrapper.sh` to seamlessly open `.torrent` files directly from your terminal file manager. 

If the daemon is offline, it spawns a new terminal window. If the daemon is already running, it pings the local REST API and silently injects the new torrent into the active background session.

**Yazi Configuration (`~/.config/yazi/yazi.toml`):**
```toml
[opener]
streamer = [ { run = '/path/to/repo/scripts/streamer-wrapper.sh "$@"', orphan = true, desc = "C++ Streamer" } ]

[open]
prepend_rules = [
    { url = "*.torrent", use = [ "streamer", "qbittorrent" ] }
]
```

### The Telemetry Dashboard & Module Tagging
Because the daemon UI is kept perfectly clean, you can monitor the real-time background network traffic by opening a second terminal window. The daemon outputs a highly structured, thread-safe trace to `streamer_debug.log`. 

The engine uses a **Module Tagging System** so you can easily `grep` for specific events:
* `[TELE]`: The 5-second P2P heartbeat showing DL speeds, connected peers, and the piece cascade.
* `[SEEK]`: Triggers the exact byte-offset and piece index when a player requests a timeline jump.
* `[PROX]` & `[HLS ]`: Traces local proxy initialization, header spoofing, and `.m3u8` generation.
* `[BLUR]`: Traces the bitwise parsing of raw Blu-ray `.clpi` index files.

**Example Usage:**
```bash
# Watch the master timeline
tail -f streamer_debug.log

# Filter ONLY for player seeks and proxy routing
grep -E "\[SEEK\]|\[PROX\]" streamer_debug.log
```

### Command Line Arguments
* `-p, --port <port>`: Change the primary local HTTP Torrent server port (Default: `8080`).
* `-d, --dir <path>`: Change the download and cache directory (Default: `/mnt/NewVolume/Tordown`).
* `--player <path>`: Define a custom path or executable for your media player (e.g., `mpv` or `vlc`). 
* `-v, --debug`: Enables verbose mode. Writes the telemetry dashboard, raw HTTP socket traces, and proxy warnings to `streamer_debug.log`.
