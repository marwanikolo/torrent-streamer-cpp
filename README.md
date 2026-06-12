# 🎬 C++ BitTorrent Streaming Daemon

![C++26](https://img.shields.io/badge/C%2B%2B-26-blue.svg)
![CMake](https://img.shields.io/badge/CMake-3.16+-green.svg)
![libtorrent](https://img.shields.io/badge/libtorrent-2.0+-orange.svg)
![Platform](https://img.shields.io/badge/Platform-Linux-lightgrey.svg)

A high-performance, concurrent BitTorrent and Direct HTTP streaming daemon built in modern C++26[cite: 8]. This application allows you to instantly stream video/audio files directly from magnet links, `.torrent` files, Direct HTTP web links, or YouTube/DASH playlists[cite: 8].

Designed as a true multi-threaded background daemon, it features an interactive CLI, a REST API Web UI, dynamic proxy routing, sequential downloading, smart bandwidth prioritization, and zero-latency Lua seeking[cite: 8].

## ✨ Killer Features

* **Zero-Copy Memory Mapping (`mmap`):** Bypasses expensive standard C++ file stream (`ifstream`) syscalls[cite: 8]. The HTTP frontend serves chunks directly from the Linux Kernel's page cache, utilizing `madvise(MADV_SEQUENTIAL)` to aggressively free RAM during massive (50GB+) ISO streaming[cite: 8].
* **Autonomous Network Sniffer (TLS Decryption):** Natively intercepts live network traffic by tapping into the `SSLKEYLOGFILE` environment variable to decrypt HTTPS streams on the fly[cite: 5]. Automatically extracts ephemeral cookies, tokens, and URL signatures from modern HTTP/2 and HTTP/3 (QUIC) traffic[cite: 5].
* **Universal Scriptable Media Proxy:** Bypasses aggressive CDN hotlink protections (like Pixeldrain or Gofile) using arbitrary HTTP header injection[cite: 4, 6]. Automatically follows cross-domain HTTP 302 redirects to locate physical storage nodes[cite: 6]. Dynamically strips carriage returns (`\r\n`) to evade Nginx tarpit connection drops[cite: 6].
* **Concurrent Multi-Streaming:** Stream multiple torrents, season packs, and direct HTTP links simultaneously[cite: 8]. The daemon dynamically allocates ports and manages isolated background threads for every active stream[cite: 8].
* **Stateless Web UI & REST API:** Features a fully integrated HTTP server hosting a stateless Web UI[cite: 8]. Add torrents, monitor active streams, and gracefully terminate processes directly from your browser via the `/api` endpoints[cite: 8].
* **Interactive Season Pack Support:** Prompts for interactive file selection on multi-file torrents[cite: 8]. Select multiple episodes (e.g., `0,1,2`) to instantly spawn multiple synchronized media player windows at once[cite: 8].
* **YouTube & DASH Integration:** Features a built-in `yt-dlp` interactive sub-shell[cite: 8]. Parses JSON manifests to extract multi-format playlists, allowing you to proxy, cache, and actively merge split video/audio tracks in real-time[cite: 8].
* **Native ISO & Blu-Ray Parsing:** Automatically detects physical disk images (`.iso`), parses their internal binary `.clpi` (Clip Information) files, maps the physical `SPN` byte offsets, and generates dynamic Apple HLS (`.m3u8`) playlists on the fly[cite: 8]. Stream 50GB+ Blu-rays sequentially over BitTorrent without extracting or transcoding[cite: 8].
* **Terminal File Manager Integration:** Includes a native bash wrapper script (`scripts/streamer-wrapper.sh`) that allows seamless "Single-Instance" remote control from terminal file managers like **Yazi**, **Ranger**, or **LF**[cite: 8].

## 🧠 Architecture & Under the Hood

This engine relies on a strictly decoupled, concurrent architecture that separates the chaotic HTTP frontend from the stable BitTorrent backend[cite: 8].

### 1. Anti-Swarm-Shock (In-Flight Shield)
Rapidly seeking across a video file normally causes standard BitTorrent clients to spam `CANCEL` messages, resulting in massive peer drops[cite: 8]. This engine uses a "Soft-Cancel" In-Flight Shield to gracefully demote active pieces to a Priority 1 Scavenger Tier, keeping TCP sockets alive and peers happy while instantly pivoting swarm bandwidth to the new playhead[cite: 8].

### 2. Thread-Safe Reference Counting & Master Sessions
Video players (`mpv`/`vlc`) send chaotic, rapidly shifting HTTP byte-range requests and keep old sockets alive to pre-cache data[cite: 8].
* **Window Manager:** Tracks piece requests across all concurrent player connections[cite: 8]. Pieces are only deprioritized when their global reference count hits zero[cite: 8].
* **Master Session Delegation:** Dynamically tracks the "Master Session" (the most recent HTTP request)[cite: 8]. The Master dictates the true playhead, granting it an aggressive `max(2, 7 - distance)` priority cascade and strict `0ms` deadlines, while instantly demoting all background threads to a passive Priority 1[cite: 8].

### 3. Zero-Latency Active Kill Seeking
Uses dynamically injected, unique Lua scripts (MPV event hooks and VLC background interfaces) to provide out-of-band signaling[cite: 8]. This proactively kills obsolete HTTP worker threads the millisecond you seek, preventing BitTorrent "phantom priority deadlocks."[cite: 8]

### 4. RAII Thread Lifecycle (C++20 `std::jthread`)
The REST API and proxy servers utilize modern `std::jthread` and `std::shared_mutex` dependency injection[cite: 8]. When the daemon receives a `SIGINT` (Ctrl+C), it seamlessly tracks and waits for all active HTTP workers to flush their I/O before safely serializing `.fastresume` data and terminating[cite: 8].

### 5. Asynchronous Interception Queue
The sniffer utilizes a background worker loop and pipes `tshark` output into a thread-safe vector queue to capture media URLs[cite: 5, 7]. It alerts the user via an ANSI escape sequence (`\r\033[K`) to seamlessly redraw the interactive prompt without screen tearing or interrupting the active typing line[cite: 7].

## 🛠️ Dependencies

* **C++26 Compiler** (GCC 14+ or Clang 18+)[cite: 8]
* **CMake** (3.16+)[cite: 8]
* **libtorrent-rasterbar** (v2.0+)[cite: 8]
* **cpp-httplib** (Header-only HTTP server)[cite: 8]
* **nlohmann/json** (For Web API & yt-dlp parsing)[cite: 8]
* **curl** (Required for out-of-band Lua seek signaling)[cite: 8]
* **yt-dlp** (For YouTube/DASH extraction)[cite: 8]
* **tshark** (Wireshark command-line utility for native network TLS decryption)[cite: 5]
* **mpv** and/or **VLC** (For playback)[cite: 8]

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

Launch the daemon via the terminal[cite: 8]. You can pass an initial link via CLI arguments, or simply start the daemon to enter the interactive REPL[cite: 8].

```bash
./build/streamer 
```

### The Interactive Daemon Prompt (`daemon>`)
Once the background servers initialize, you will be dropped into the interactive command loop[cite: 8].

* `add <link>`: Paste a magnet link, local `.torrent` path, or HTTP link[cite: 8]. Supports inline dynamic flag overrides per-command (e.g., `add <link> --referer <url>`)[cite: 7].
* `yt`: Enters the interactive `yt-dlp` sub-shell[cite: 8]. Use `-J` or `--dump-json` to extract format lists and select specific video/audio streams to proxy[cite: 8].
* `sniff start`: Starts the native network interception queue via `tshark`[cite: 7].
* `sniff list`: Displays captured media streams alongside their capture timestamps[cite: 7].
* `sniff play <idx>`: Plays an intercepted stream[cite: 7]. Supports multi-target launching (e.g., `sniff play 0,1`) to actively merge split video and audio[cite: 7].
* `sniff stop / clear`: Stops the background sniffer process or clears the interception queue[cite: 7].
* `list`: Displays a real-time list of all active BitTorrent and Direct Web streams[cite: 8].
* `stop <hash/url>`: Gracefully terminates a specific active stream and saves `.fastresume` data[cite: 8].
* `peer <hash> <ip>:<port>`: Manually injects a specific peer (e.g., from a private tracker) directly into the libtorrent swarm via a raw TCP/µTP handshake[cite: 8].
* `quit`: Safely kills all active MPV/VLC windows, terminates all network connections, saves global fastresume data, and shuts down the daemon[cite: 8].

### Yazi / Terminal File Manager Integration
You can use the provided `scripts/streamer-wrapper.sh` to seamlessly open `.torrent` files directly from your terminal file manager[cite: 8]. 

If the daemon is offline, it spawns a new terminal window[cite: 8]. If the daemon is already running, it pings the local REST API and silently injects the new torrent into the active background session[cite: 8].

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
Because the daemon UI is kept perfectly clean, you can monitor the real-time background network traffic by opening a second terminal window[cite: 8]. The daemon outputs a highly structured, thread-safe trace to `streamer_debug.log`[cite: 8]. 

The engine uses a **Module Tagging System** so you can easily `grep` for specific events[cite: 8]:
* `[TELE]`: The 5-second P2P heartbeat showing DL speeds, connected peers, and the piece cascade[cite: 8].
* `[SEEK]`: Triggers the exact byte-offset and piece index when a player requests a timeline jump[cite: 8].
* `[PROX]` & `[HLS ]`: Traces local proxy initialization, header spoofing, and `.m3u8` generation[cite: 8].
* `[BLUR]`: Traces the bitwise parsing of raw Blu-ray `.clpi` index files[cite: 8].

**Example Usage:**
```bash
# Watch the master timeline
tail -f streamer_debug.log

# Filter ONLY for player seeks and proxy routing
grep -E "\[SEEK\]|\[PROX\]" streamer_debug.log
```

### Command Line Arguments
* `-p, --port <port>`: Change the primary local HTTP Torrent server port (Default: `8080`)[cite: 8].
* `-d, --dir <path>`: Change the download and cache directory (Default: `/mnt/NewVolume/Tordown`)[cite: 8].
* `--player <path>`: Define a custom path or executable for your media player (e.g., `mpv` or `vlc`)[cite: 8]. 
* `-v, --debug`: Enables verbose mode[cite: 8]. Writes the telemetry dashboard, raw HTTP socket traces, and proxy warnings to `streamer_debug.log`[cite: 8].
* `--user-agent <string>`: Spoofs a custom User-Agent for direct HTTP links[cite: 7].
* `--referer <url>`: Spoofs a custom Referer for direct HTTP links[cite: 7].
* `-H, --header <string>`: Passes arbitrary HTTP headers (e.g., "Authorization: Bearer...") for dynamic proxy spoofing[cite: 7].
* `--gofile-token <string>`: Injects a static token explicitly used for Gofile bypasses[cite: 7].
