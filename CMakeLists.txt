cmake_minimum_required(VERSION 3.16)
project(TorrentStreamer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find necessary system libraries
find_package(LibtorrentRasterbar REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# Define our source files
set(SOURCES
    main.cpp
    ProcessManager.cpp
    MediaParser.cpp
    TorrentEngine.cpp
)

# Create the executable
add_executable(streamer ${SOURCES})

# Link libraries (Using the strict LibtorrentRasterbar namespace)
target_link_libraries(streamer 
    PRIVATE 
    LibtorrentRasterbar::torrent-rasterbar 
    OpenSSL::SSL 
    OpenSSL::Crypto 
    Threads::Threads
)
