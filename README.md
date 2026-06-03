# C++ BitTorrent Streaming Engine

A high-performance, multi-threaded BitTorrent streaming engine built in C++. This application allows you to instantly stream video/audio files directly from magnet links or `.torrent` files using sequential downloading, smart bandwidth prioritization, and a robust HTTP streaming backend.

## ✨ Features

* **Lightning-Fast Sequential Streaming:** Built on `libtorrent-rasterbar`, forcing sequential piece downloading for instant playback.
* **Smart Sliding Piece Priorities:** Dynamically assigns `libtorrent` piece priorities (7 down to 1) and enforces time-critical deadlines (200ms) for the immediate playback head, preventing swarm choking.
* **Multi-Threaded HTTP Range Server:** Powered by `cpp-httplib` with a 64-thread pool. Natively handles HTTP `Range` requests, allowing perfect seeking without breaking socket connections.
* **Blu-Ray Instant Seeking (HLS):** Automatically detects `.m2ts` files, downloads their associated `.clpi` index maps, and generates a virtual `#EXTM3U` HLS playlist in RAM to allow instant, skip-free seeking across massive Blu-ray structures.
* **Player Protection:** Automatically launches `mpv` or `vlc` with heavily optimized caching parameters. Specifically detects and isolates MPV's `thumbfast` script to prevent background thumbnail generation from DDoS'ing the active swarm bandwidth.
* **Graceful Shutdown & Fastresume:** Traps `SIGINT` (Ctrl+C) to safely kill the video player, release sockets, and serialize `.fastresume` data to disk, allowing instant re-launches without hash-checking.

## 🛠️ Dependencies

* **C++20** (Requires a modern compiler)
* **CMake** (Build system)
* **libtorrent-rasterbar** (v2.0+)
* **cpp-httplib** (Header-only HTTP server)
* **mpv** or **VLC** (For playback)

## 🚀 Building from Source

```bash
# Clone the repository
git clone [https://github.com/](https://github.com/)<YOUR_USERNAME>/<YOUR_REPO_NAME>.git
cd <YOUR_REPO_NAME>

# Create build directory
mkdir build && cd build

# Configure and compile
cmake ..
make
```

## 🎮 Usage

Launch the engine via the terminal. You can pass a magnet link or a `.torrent` file directly, or start the engine and paste it into the interactive prompt.

```bash
./streamer "magnet:?xt=urn:btih:..." 
```

### Command Line Arguments
* `-p, --port <port>`: Change the local HTTP server port (Default: 9999)
* `-d, --dir <path>`: Change the download/cache directory
* `--mpv`: Launch stream in MPV with optimized RAM caching (Default).
* `--vlc`: Launch stream in VLC with optimized network caching.
* `--player <path>`: Define a custom path to your media player.
* `--debug, -v`: Enable verbose logging to `streamer_debug.log`.
