cmake_minimum_required(VERSION 3.16)
project(TorrentStreamer)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- ADD THIS: Default to RelWithDebInfo if no build type is specified ---
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting build type to 'RelWithDebInfo' as none was specified.")
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE
      STRING "Choose the type of build." FORCE)
endif()
# -----------------------------------------------------------------------

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

find_package(LibtorrentRasterbar REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

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

# --- ADD THIS: Ensure frame pointers are kept for the profiler ---
target_compile_options(streamer PRIVATE -fno-omit-frame-pointer)
