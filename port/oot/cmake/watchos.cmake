# Dependencies of Ship of Harkinian and libultraship on watchOS. Included by
# Ship of Harkinian's top-level CMakeLists.txt (patches/oot) in place of its
# Torch and asset-tool setup when CMAKE_SYSTEM_NAME is watchOS.

include(FetchContent)

#=================== nlohmann-json, tinyxml2, spdlog ===================
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(nlohmann_json)

set(tinyxml2_BUILD_TESTING OFF)
FetchContent_Declare(
    tinyxml2
    GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git
    GIT_TAG 11.0.0
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(tinyxml2)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.16.0
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(spdlog)

#=================== libzip ===================
# Reads the .o2r archives. zlib comes with the SDK; nothing else is needed.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(BUILD_TOOLS OFF)
set(BUILD_REGRESS OFF)
set(BUILD_OSSFUZZ OFF)
set(BUILD_EXAMPLES OFF)
set(BUILD_DOC OFF)
set(BUILD_SHARED_LIBS OFF)
set(ENABLE_COMMONCRYPTO OFF)
set(ENABLE_GNUTLS OFF)
set(ENABLE_MBEDTLS OFF)
set(ENABLE_OPENSSL OFF)
set(ENABLE_WINDOWS_CRYPTO OFF)
set(ENABLE_BZIP2 OFF)
set(ENABLE_LZMA OFF)
set(ENABLE_ZSTD OFF)
FetchContent_Declare(
    libzip
    GIT_REPOSITORY https://github.com/nih-at/libzip.git
    GIT_TAG v1.11.4
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(libzip)

#=================== Ogg, Vorbis, Opus ===================
# For custom music. Plain C, built with their own CMake (old enough to need
# CMake's compatibility policies).
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(INSTALL_DOCS OFF)
set(BUILD_TESTING OFF)
FetchContent_Declare(
    Ogg
    GIT_REPOSITORY https://github.com/xiph/ogg.git
    GIT_TAG v1.3.5
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(Ogg)

FetchContent_Declare(
    Vorbis
    GIT_REPOSITORY https://github.com/xiph/vorbis.git
    GIT_TAG v1.3.7
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(Vorbis)

set(OPUS_BUILD_PROGRAMS OFF)
set(OPUS_BUILD_TESTING OFF)
FetchContent_Declare(
    Opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.5.2
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(Opus)

# opusfile 0.12 has no CMake build; it is four C files.
FetchContent_Declare(
    OpusFile
    GIT_REPOSITORY https://github.com/xiph/opusfile.git
    GIT_TAG v0.12
    SOURCE_SUBDIR watch-port-no-cmake
)
FetchContent_MakeAvailable(OpusFile)
add_library(opusfile STATIC
    ${opusfile_SOURCE_DIR}/src/info.c
    ${opusfile_SOURCE_DIR}/src/internal.c
    ${opusfile_SOURCE_DIR}/src/opusfile.c
    ${opusfile_SOURCE_DIR}/src/stream.c
)
target_include_directories(opusfile PUBLIC ${opusfile_SOURCE_DIR}/include)
target_link_libraries(opusfile PUBLIC ogg opus)
target_compile_options(opusfile PRIVATE -w)

#=================== SDL2 ===================
# SDL has no watchOS port, but the game and libultraship only need its core:
# events, threads, timers and the game controller bookkeeping. That core is
# built here with the dummy video, audio and joystick drivers and a
# configuration of our own (sdl/SDL_config.h); the watch app does display,
# sound and input itself.
FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.32.10
    SOURCE_SUBDIR watch-port-no-cmake # fetch only, SDL's own build does not support watchOS
)
FetchContent_MakeAvailable(SDL2)

# SDL's headers with our SDL_config.h, reachable as <SDL.h> and <SDL2/SDL.h>.
set(SDL2_WATCH_INCLUDE ${CMAKE_BINARY_DIR}/sdl2-watch/include)
file(GLOB SDL2_HEADERS ${sdl2_SOURCE_DIR}/include/*.h)
file(COPY ${SDL2_HEADERS} DESTINATION ${SDL2_WATCH_INCLUDE}/SDL2)
configure_file(${CMAKE_CURRENT_LIST_DIR}/../sdl/SDL_config.h ${SDL2_WATCH_INCLUDE}/SDL2/SDL_config.h COPYONLY)
set(SDL2_INCLUDE_DIRS ${SDL2_WATCH_INCLUDE} ${SDL2_WATCH_INCLUDE}/SDL2)

set(SDL2_SRC ${sdl2_SOURCE_DIR}/src)
file(GLOB SDL2_SOURCES
    ${SDL2_SRC}/*.c
    ${SDL2_SRC}/atomic/*.c
    ${SDL2_SRC}/audio/*.c
    ${SDL2_SRC}/audio/dummy/*.c
    ${SDL2_SRC}/cpuinfo/*.c
    ${SDL2_SRC}/events/*.c
    ${SDL2_SRC}/file/*.c
    ${SDL2_SRC}/filesystem/dummy/*.c
    ${SDL2_SRC}/haptic/*.c
    ${SDL2_SRC}/haptic/dummy/*.c
    ${SDL2_SRC}/joystick/*.c
    ${SDL2_SRC}/joystick/dummy/*.c
    ${SDL2_SRC}/libm/*.c
    ${SDL2_SRC}/loadso/dummy/*.c
    ${SDL2_SRC}/locale/*.c
    ${SDL2_SRC}/locale/dummy/*.c
    ${SDL2_SRC}/misc/*.c
    ${SDL2_SRC}/misc/dummy/*.c
    ${SDL2_SRC}/power/*.c
    ${SDL2_SRC}/render/*.c
    ${SDL2_SRC}/render/software/*.c
    ${SDL2_SRC}/sensor/*.c
    ${SDL2_SRC}/sensor/dummy/*.c
    ${SDL2_SRC}/stdlib/*.c
    ${SDL2_SRC}/thread/*.c
    ${SDL2_SRC}/thread/pthread/*.c
    ${SDL2_SRC}/timer/*.c
    ${SDL2_SRC}/timer/unix/*.c
    ${SDL2_SRC}/video/*.c
    ${SDL2_SRC}/video/dummy/*.c
    ${SDL2_SRC}/video/yuv2rgb/*.c
)
add_library(SDL2-static STATIC ${SDL2_SOURCES} ${CMAKE_CURRENT_LIST_DIR}/../sdl/SDL_watch.c)
target_include_directories(SDL2-static PUBLIC ${SDL2_INCLUDE_DIRS})
target_include_directories(SDL2-static PRIVATE ${SDL2_SRC})
target_compile_options(SDL2-static PRIVATE -w)
add_library(SDL2::SDL2-static ALIAS SDL2-static)
add_library(SDL2::SDL2 ALIAS SDL2-static)
add_library(SDL2::SDL2main INTERFACE IMPORTED)
# On other platforms SDL's headers reach libultraship through its ImGui target.
include_directories(${SDL2_INCLUDE_DIRS})
set(SDL2_FOUND TRUE)
# libultraship and the game find SDL2 with find_package(); these say it is here.
set(SDL2_DIR ${CMAKE_BINARY_DIR}/sdl2-watch)
file(WRITE ${SDL2_DIR}/SDL2Config.cmake "set(SDL2_FOUND TRUE)\nset(SDL2_INCLUDE_DIRS \"${SDL2_INCLUDE_DIRS}\")\n")

#=================== SDL2_net ===================
# For Ship of Harkinian's online features. They stay off on the watch, but the
# game's code refers to them; BSD sockets compile on watchOS.
FetchContent_Declare(
    SDL2_net
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_net.git
    GIT_TAG release-2.2.0
    SOURCE_SUBDIR watch-port-no-cmake
)
FetchContent_MakeAvailable(SDL2_net)
file(COPY ${sdl2_net_SOURCE_DIR}/SDL_net.h DESTINATION ${SDL2_WATCH_INCLUDE}/SDL2)
file(GLOB SDL2_NET_SOURCES ${sdl2_net_SOURCE_DIR}/SDLnet*.c)
add_library(SDL2_net-static STATIC ${SDL2_NET_SOURCES})
target_include_directories(SDL2_net-static PUBLIC ${SDL2_INCLUDE_DIRS})
target_compile_options(SDL2_net-static PRIVATE -w)
add_library(SDL2_net::SDL2_net ALIAS SDL2_net-static)
set(SDL2_net_DIR ${CMAKE_BINARY_DIR}/sdl2-watch)
file(WRITE ${SDL2_net_DIR}/SDL2_netConfig.cmake "set(SDL2_net_FOUND TRUE)\nset(SDL_NET_INCLUDE_DIRS \"${SDL2_INCLUDE_DIRS}\")\n")
