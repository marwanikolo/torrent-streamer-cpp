cmake_minimum_required(VERSION 3.16)
project(TorrentStreamer)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- NEW: Fix for CMake 3.30+ FindBoost deprecation warning ---
# Must go BEFORE LibtorrentRasterbar, since it internally triggers FindBoost
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

# Find necessary system libraries
find_package(LibtorrentRasterbar REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# Define our source files
set(SOURCES
    main.cpp 
    TorrentEngine.cpp 
    WindowManager.cpp
    AlertHandler.cpp
    HttpServer.cpp
    Utils.cpp 
    ProcessManager.cpp 
    MediaParser.cpp
    HttpCacheManager.cpp
    HttpDownloader.cpp
    HttpProxyServer.cpp
    DirectLinkEngine.cpp
    YtdlpWrapper.cpp
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

# Tell cpp-httplib to enable HTTPS and use OpenSSL
target_compile_definitions(streamer PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
