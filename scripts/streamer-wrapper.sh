#!/bin/bash

# ==============================================================================
# C++ Streamer - Terminal File Manager Wrapper
# ==============================================================================

TORRENT_FILE="$1"
PORT=9999 # Matched to your Config.h!

# ABSOLUTE paths are strictly required because Yazi executes this from 
# different working directories depending on where you are browsing!
PROJECT_DIR="/home/marwan/torrent-streamer-cpp"
STREAMER_BIN="$PROJECT_DIR/build/streamer"

# Convert target to an absolute path
ABS_PATH=$(realpath "$TORRENT_FILE")
API_BASE="http://127.0.0.1:$PORT/api"

# 1. Check if the 'streamer' process is running at all
if pgrep -x "streamer" > /dev/null; then
    
    # Daemon is booting or running! Wait up to 3 seconds for the HTTP API to respond
    for i in {1..30}; do
        if curl --silent --fail "$API_BASE/status" > /dev/null; then
            
            # API is ready. Inject the new torrent!
            JSON_PAYLOAD="{\"url\": \"$ABS_PATH\"}"
            curl -s -X POST -H "Content-Type: application/json" -d "$JSON_PAYLOAD" "$API_BASE/play/torrent" > /dev/null
            
            notify-send "C++ Streamer" "Added new torrent to active daemon!"
            exit 0
        fi
        sleep 0.1 
    done
    
    notify-send "C++ Streamer Error" "Daemon is running but API did not respond on Port $PORT."
else
    # 2. Daemon is completely offline. Spawn it in a new terminal.
    # Writing the command directly here avoids all bash quote-splitting bugs!
    kitty @ launch --type=tab --title "C++_Streamer" --cwd="$PROJECT_DIR" "$STREAMER_BIN" "$ABS_PATH" &
fi
