# C++ BitTorrent Streaming Engine

A high-performance, multi-threaded BitTorrent and Direct HTTP streaming engine built in C++. This application allows you to instantly stream video/audio files directly from magnet links, `.torrent` files, or Direct HTTP web links. It utilizes a highly decoupled proxy architecture, sequential downloading, smart bandwidth prioritization, and zero-latency Lua seeking.

## ✨ Features

* **Lightning-Fast Sequential Streaming:** Built on `libtorrent-rasterbar`, forcing sequential piece downloading for instant playback of massive swarms.
* **Zero-Latency Active Kill Seeking:** Uses dynamically injected Lua scripts (MPV event hooks and VLC background interfaces) to provide out-of-band signaling. This proactively kills obsolete HTTP worker threads the millisecond you seek, preventing BitTorrent "phantom priority deadlocks."
* **Native ISO & Blu-Ray VFS Mounting:** Automatically detects `.iso` files, routes playback to VLC, and disables BD-J (Java) menus to seamlessly stream 50GB+ physical disk images over BitTorrent using VLC's Virtual File System.
* **Universal Direct HTTP Engine:** Paste any standard video URL to proxy, cache, and stream it through the C++ engine. Features aggressive CDN bypass (HEAD to GET partial range fallbacks).
* **Blu-Ray Instant Seeking (HLS):** Automatically detects BDMV `.m2ts` files, downloads their associated `.clpi` index maps, and generates a virtual `#EXTM3U` HLS playlist in RAM to allow instant, skip-free seeking across Blu-ray structures.
* **Player Protection & Timeout Hardening:** Automatically launches `mpv` or `vlc` with heavily optimized caching parameters. Fixes MKV metadata drops via aggressive network timeouts (1200s).
* **Graceful Shutdown & Fastresume:** Traps `SIGINT` (Ctrl+C) to safely serialize `.fastresume` data to disk, allowing instant re-launches without hash-checking.

## 🧠 Architecture & Under the Hood

This engine relies on a strictly decoupled, two-tier architecture that completely separates the chaotic HTTP frontend from the stable BitTorrent backend. 

### The Sliding Window Manager
Video players (`mpv`/`vlc`) send chaotic, rapidly shifting HTTP byte-range requests. If these were passed directly to the swarm, the BitTorrent engine would choke. Instead, requests pass through the **Window Manager**:
1. **Reference Counting:** The manager tracks overlapping HTTP requests to ensure pieces are only deprioritized when *zero* active threads need them.
2. **Priority Scaling:** Piece priority dynamically scales based on proximity to the playhead using the formula `max(1, 7 - distance)`. The exact piece being watched is set to priority `7` (Highest), while pieces further down the timeline taper off to priority `1`.
3. **Aggressive Deadlines:** To prevent buffering, the manager enforces strict time-to-live deadlines on `libtorrent`. The immediate 4 pieces are assigned severe `200ms` deadlines, forcing peers to drop non-critical uploads and instantly satisfy the playback buffer. The remaining buffer window is given relaxed `1000ms` deadlines.
4. **Header/Footer Pinning:** Piece `0` and the final piece of the file are permanently locked to `top_priority`. This guarantees that MKV `Cues`, MP4 `Moov` atoms, and ISO `UDF` anchors are always instantly available for fast seeking.

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
make
```

## 🎮 Usage & Advanced Configuration

Launch the engine via the terminal. You can pass a source directly via CLI or start the engine and paste it into the interactive prompt.

```bash
# Stream a Magnet Link
./streamer "magnet:?xt=urn:btih:..." 

# Stream a local Torrent file
./streamer "/path/to/movie.torrent"

# Stream a Direct web link
./streamer "[https://example.com/video.mp4](https://example.com/video.mp4)"
```

### Command Line Arguments
* `-p, --port <port>`: Change the local HTTP server proxy port (Default: System assigned, usually 9999).
* `-d, --dir <path>`: Change the download and cache directory (Default: `/mnt/NewVolume/Tordown`).
* `--player <path>`: Define a custom path or executable for your media player (e.g., `mpv` or `vlc`). *Note: The engine will dynamically override this to `vlc` if an `.iso` file is selected to ensure VFS support.*
* `-v, --debug`: Enables verbose mode. Writes raw HTTP socket traces, BitTorrent peer states, and proxy warnings to `streamer_debug.log`.
