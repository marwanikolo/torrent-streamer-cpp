cmake_minimum_required(VERSION 3.16)
project(TorrentStreamer)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

find_package(LibtorrentRasterbar REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# --- ADDED THE 3 NEW ARCHITECTURE FILES ---
set(SOURCES
    main.cpp 
    CliParser.cpp
    StreamerDaemon.cpp
    InteractiveShell.cpp
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
    NetworkSniffer.cpp
)

add_executable(streamer ${SOURCES})

target_link_libraries(streamer 
    PRIVATE 
    LibtorrentRasterbar::torrent-rasterbar 
    OpenSSL::SSL 
    OpenSSL::Crypto 
    Threads::Threads
)

target_compile_definitions(streamer PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
