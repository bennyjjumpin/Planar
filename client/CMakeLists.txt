cmake_minimum_required(VERSION 3.24)
project(music_player_client LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
    # wxWebView's Edge (WebView2) backend is only reliable with MSVC. MinGW
    # builds silently fall back to the IE backend, which YouTube doesn't support.
    message(WARNING "This project targets MSVC on Windows; other toolchains are untested.")
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------------------
# wxWidgets (static, with the Edge/WebView2 webview backend)
# ---------------------------------------------------------------------------
# The release tarball already contains wx's git submodules, so it's much faster
# than a recursive git clone.
FetchContent_Declare(wxWidgets
    URL https://github.com/wxWidgets/wxWidgets/releases/download/v3.2.9/wxWidgets-3.2.9.tar.bz2
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(wxBUILD_SHARED OFF CACHE BOOL "" FORCE)
set(wxUSE_WEBVIEW ON CACHE BOOL "" FORCE)
set(wxUSE_WEBVIEW_EDGE ON CACHE BOOL "" FORCE)       # wx downloads the WebView2 SDK from NuGet itself
set(wxUSE_WEBVIEW_EDGE_STATIC ON CACHE BOOL "" FORCE) # link the loader statically: no WebView2Loader.dll to ship
# Libraries we don't use. Turning them off cuts the first build time noticeably.
foreach(opt wxUSE_STC wxUSE_RIBBON wxUSE_PROPGRID wxUSE_RICHTEXT wxUSE_GLCANVAS wxUSE_MEDIACTRL wxUSE_XRC wxUSE_AUI)
    set(${opt} OFF CACHE BOOL "" FORCE)
endforeach()

# ---------------------------------------------------------------------------
# mbedTLS (TLS backend for IXWebSocket, needed for wss:// to Cloudflare)
# ---------------------------------------------------------------------------
# IXWebSocket defaults to mbedTLS on Windows. It reads the Windows certificate
# store itself, so no CA bundle file is needed.
FetchContent_Declare(mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)

# ---------------------------------------------------------------------------
# IXWebSocket
# ---------------------------------------------------------------------------
FetchContent_Declare(ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG v12.1.0
    GIT_SHALLOW TRUE
)
set(USE_TLS ON CACHE BOOL "" FORCE)
set(USE_MBED_TLS ON CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE) # per-message deflate isn't worth it for tiny JSON messages
# We never install it, and its export set can't reference our fetched mbedTLS targets.
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)

# ---------------------------------------------------------------------------
# nlohmann/json
# ---------------------------------------------------------------------------
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(wxWidgets mbedtls nlohmann_json)

# IXWebSocket would normally find_package(MbedTLS), which doesn't work for a
# FetchContent'd mbedTLS. Its CMakeLists honours these variables instead.
set(MBEDTLS_FOUND TRUE)
set(MBEDTLS_INCLUDE_DIRS "${mbedtls_SOURCE_DIR}/include")
set(MBEDTLS_LIBRARIES mbedtls mbedx509 mbedcrypto)
FetchContent_MakeAvailable(ixwebsocket)
# Normally set by IXWebSocket after its own version check, which we bypassed.
target_compile_definitions(ixwebsocket PRIVATE IXWEBSOCKET_USE_MBED_TLS_MIN_VERSION_3)

# ---------------------------------------------------------------------------
# The app
# ---------------------------------------------------------------------------
add_executable(music_player WIN32
    src/App.cpp
    src/MainFrame.cpp
    src/YouTubePlayer.cpp
    src/SessionState.cpp
    src/ClockSync.cpp
    src/PlaybackController.cpp
    src/SessionConnection.cpp
    src/RelayClient.cpp
    src/Logo.cpp
    src/app.rc
)
target_include_directories(music_player PRIVATE
    src
    # wx keeps the WebView2 SDK headers private; we need them to set up the
    # virtual host mapping (see YouTubePlayer.cpp).
    "${WEBVIEW2_PACKAGE_DIR}/build/native/include"
)
target_link_libraries(music_player PRIVATE
    wx::webview wx::core wx::base
    ixwebsocket
    nlohmann_json::nlohmann_json
)
target_compile_definitions(music_player PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# The CMake target keeps its original name (CLion run configurations refer
# to it); the program itself is Planar.exe.
set_target_properties(music_player PROPERTIES OUTPUT_NAME "Planar")

# The player page is served from <exe dir>/web, so copy it there on every build.
add_custom_command(TARGET music_player POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/web" "$<TARGET_FILE_DIR:music_player>/web"
    COMMENT "Copying web/ next to the executable"
)
