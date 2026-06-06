# C++ BitTorrent Streaming Daemon

A high-performance, concurrent BitTorrent and Direct HTTP streaming daemon built in C++. This application allows you to instantly stream video/audio files directly from magnet links, `.torrent` files, or Direct HTTP web links. 

Recently rewritten into a true multi-threaded background daemon, it features an interactive CLI, dynamic proxy routing, sequential downloading, smart bandwidth prioritization, and zero-latency Lua seeking.

## ✨ Features

* **Concurrent Multi-Streaming:** Stream multiple torrents, season packs, and direct HTTP links simultaneously. The daemon dynamically allocates ports and manages isolated background threads for every active stream.
* **Interactive Season Pack Support:** Prompts for interactive file selection on multi-file torrents. Select multiple episodes (e.g., `0,1,2`) to instantly spawn multiple synchronized media player windows at once.
* **Headless Daemon CLI:** Features a clean, non-blocking interactive prompt (`daemon>`). All HTTP request traces, chunk cache updates, and P2P piece alerts are piped safely to an isolated `streamer_debug.log` to keep your terminal perfectly clean.
* **Lightning-Fast Sequential Streaming:** Built on `libtorrent-rasterbar`, forcing sequential piece downloading for instant playback of massive swarms.
* **Zero-Latency Active Kill Seeking:** Uses dynamically injected, unique Lua scripts (MPV event hooks and VLC background interfaces) to provide out-of-band signaling. This proactively kills obsolete HTTP worker threads the millisecond you seek, preventing BitTorrent "phantom priority deadlocks."
* **Universal Direct HTTP Engine:** Paste any standard video URL to proxy, cache, and stream it through the C++ engine. Features dynamic port binding to prevent collision with torrent traffic.
* **Native ISO & Blu-Ray VFS Mounting:** Automatically detects `.iso` files, routes playback to VLC, and disables BD-J (Java) menus to seamlessly stream 50GB+ physical disk images over BitTorrent using VLC's Virtual File System.
* **Graceful Shutdown:** Traps `SIGINT` (Ctrl+C) and utilizes a multi-kill TCP deadlock unblocker to instantly terminate all spawned media players and serialize `.fastresume` data to disk for instant re-launches.

## 🧠 Architecture & Under the Hood

This engine relies on a strictly decoupled, concurrent architecture that separates the chaotic HTTP frontend from the stable BitTorrent backend.

### 1. The Central Demultiplexer Registry
The daemon runs a global `TorrentManager` backed by a `std::shared_mutex` read-write lock. When Libtorrent alerts the system that a piece has finished downloading, the background router inspects the `info_hash`, checks the central registry, and wakes up *only* the specific HTTP threads waiting for that exact torrent file. 

### 2. The Sliding Window Manager
Video players (`mpv`/`vlc`) send chaotic, rapidly shifting HTTP byte-range requests. If these were passed directly to the swarm, the BitTorrent engine would choke. Instead, requests pass through the **Window Manager**:
* **Priority Scaling:** Piece priority dynamically scales based on proximity to the playhead using the formula `max(1, 7 - distance)`. The exact piece being watched is set to priority `4` (Highest), while pieces further down the timeline taper off.
* **Aggressive Deadlines:** To prevent buffering, the manager enforces strict time-to-live deadlines on `libtorrent`. The immediate window is assigned severe `200ms` deadlines, forcing peers to drop non-critical uploads and instantly satisfy the playback buffer.

## 🛠️ Dependencies

* **C++20** (Requires a modern compiler: GCC 11+ or Clang 13+)
* **CMake** (Build system 3.16+)
* **libtorrent-rasterbar** (v2.0+)
* **cpp-httplib** (Header-only HTTP server)
* **curl** (Required for out-of-band Lua seek signaling)
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
./streamer 
```

### The Interactive Daemon Prompt (`daemon>`)
Once the background servers initialize, you will be dropped into the interactive command loop.

* `add <link>`: Paste a magnet link, local `.torrent` path, or HTTP `.mp4` link. The daemon will automatically parse it, launch the background proxies, and pop open your video player. (Quotes around the link are automatically safely stripped).
* `list`: Displays a real-time list of all active streams, showing their file paths and local HTTP proxy URLs.
* `quit`: Safely kills all active MPV/VLC windows, terminates all network connections, saves fastresume data, and shuts down the daemon.

### Monitoring Logs
Because the daemon UI is kept perfectly clean, you can monitor the real-time background network traffic by opening a second terminal window and running:
```bash
tail -f streamer_debug.log
```

### Command Line Arguments
* `-p, --port <port>`: Change the primary local HTTP Torrent server port (Default: `8080`). Direct HTTP links will dynamically allocate ports sequentially above this number.
* `-d, --dir <path>`: Change the download and cache directory (Default: `/mnt/NewVolume/Tordown`).
* `--player <path>`: Define a custom path or executable for your media player (e.g., `mpv` or `vlc`). *Note: The engine will dynamically override this to `vlc` if an `.iso` file is selected to ensure VFS support.*
* `-v, --debug`: Enables verbose mode. Writes raw HTTP socket traces, BitTorrent peer states, and proxy warnings to `streamer_debug.log`.
