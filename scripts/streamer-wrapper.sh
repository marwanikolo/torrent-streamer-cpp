#!/bin/bash

# ==============================================================================
# C++ Streamer - Terminal File Manager Wrapper
# 
# This script allows terminal file managers (like Yazi, Ranger, LF) to interact 
# with the daemon. If the daemon is running, it silently injects the torrent 
# via the REST API. If offline, it spawns a new terminal window.
# ==============================================================================

TORRENT_FILE="$1"
PORT=9999 # Match this to the port in your Config.h!

# Set your preferred terminal emulator and the path to your compiled binary
# Examples: 
#   TERMINAL_CMD="alacritty -e"
#   TERMINAL_CMD="kitty @ launch --type=tab --title 'C++ Streamer' --"
TERMINAL_CMD="kitty @ launch --type=tab --title 'C++ Streamer' --"
STREAMER_BIN="./build/streamer" # Change this if your binary is located elsewhere

# Convert to an absolute path
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
    $TERMINAL_CMD $STREAMER_BIN "$ABS_PATH" &
fi
