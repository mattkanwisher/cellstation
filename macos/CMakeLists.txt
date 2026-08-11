# CellStation macOS superproject — a self-owned, Qt-free frontend over the SAME
# rpcs3_emu core the Android app uses.
#
# rpcs3's monolithic top-level CMakeLists assumes it is the root project and, on
# non-Android hosts, unconditionally pulls in Qt and builds the desktop app
# (rpcs3qt). We don't want any of that. Instead this file reproduces the subset
# of rpcs3's top-level + app CMake that the *core* needs, and embeds only its
# `3rdparty` and `Emu` (rpcs3_emu) subdirectories — no Qt, no rpcs3qt, no app.
# Then it adds our own C-ABI bridge + CLI harness.
#
# The two embedded subdirs are clean under this arrangement: neither
# 3rdparty/CMakeLists.txt nor Emu/CMakeLists.txt references CMAKE_SOURCE_DIR
# (Emu uses the patched PROJECT_SOURCE_DIR; the Find modules are patched to
# anchor on their own location — see patches/0001).
#
# Configure/build: see macos/build-macos.sh.

cmake_minimum_required(VERSION 3.28)

find_program(CCACHE_PATH ccache HINTS ENV PATH)
if(CCACHE_PATH)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PATH}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PATH}")
endif()

project(cellstation_macos LANGUAGES C CXX OBJC OBJCXX)

if(NOT APPLE)
    message(FATAL_ERROR "This superproject is macOS-only.")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.0)
    message(FATAL_ERROR "rpcs3 requires clang >= 19 (Apple clang is too old; use `brew install llvm`).")
endif()

# Normalized (no "..") so it string-matches CMAKE_CURRENT_LIST_DIR inside
# rpcs3's Find modules — FindZLIB temporarily REMOVE_ITEMs its own dir from
# CMAKE_MODULE_PATH to reach the system module, and that match must be exact.
get_filename_component(RPCS3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../rpcs3" ABSOLUTE)

# ---- our own frontend option ------------------------------------------------
# Headless bring-up (M1/M2) uses the Null renderer, so Vulkan/MoltenVK is off by
# default. Flip ON for the windowed Metal path (M3).
option(CELLSTATION_WITH_VULKAN "Build the Vulkan (MoltenVK) render path" OFF)

# ---- bring-up overrides (win over the option() defaults below) --------------
# WITH_LLVM=OFF -> interpreter-only: the reference execution path, LLVM-free and
# fast to build. Perf work stays on the Snapdragon device, not here.
set(WITH_LLVM               OFF CACHE BOOL "" FORCE)
set(USE_NATIVE_INSTRUCTIONS OFF CACHE BOOL "" FORCE)
set(USE_SDL                 OFF CACHE BOOL "" FORCE)
set(USE_FAUDIO              OFF CACHE BOOL "" FORCE)
set(USE_LIBEVDEV            OFF CACHE BOOL "" FORCE)
set(USE_DISCORD_RPC         OFF CACHE BOOL "" FORCE)
set(USE_GAMEMODE            OFF CACHE BOOL "" FORCE)
set(USE_LTO                 OFF CACHE BOOL "" FORCE)
set(USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(BUILD_RPCS3_TESTS       OFF CACHE BOOL "" FORCE)
# We use Homebrew ffmpeg (8.0 >= rpcs3's 7.1 floor) rather than vendoring the
# ffmpeg prebuilt submodule.
set(USE_SYSTEM_FFMPEG       ON  CACHE BOOL "" FORCE)
# Build the vendored openal-soft (rpcs3 expects its bare headers, e.g. alext.h,
# which the system OpenAL.framework doesn't provide as bare includes). Disable
# its RTKit backend: it find_package's Homebrew DBus1, which puts
# -I/opt/homebrew/include ahead of openal-soft's bundled fmt-11.2.0 and makes
# <fmt/base.h> resolve to Homebrew's fmt (breaks hrtf_loader.cpp). RTKit is a
# Linux-only power/priority helper; we don't need it.
set(USE_SYSTEM_OPENAL       OFF CACHE BOOL "" FORCE)
set(ALSOFT_RTKIT            OFF CACHE BOOL "" FORCE)
if(CELLSTATION_WITH_VULKAN)
    set(USE_VULKAN ON  CACHE BOOL "" FORCE)
else()
    set(USE_VULKAN OFF CACHE BOOL "" FORCE)
endif()

# ---- rpcs3's option set (verbatim; overrides above already win) -------------
if(APPLE OR WIN32)
    set(USE_SYSTEM_OPENAL_DEFAULT OFF)
else()
    set(USE_SYSTEM_OPENAL_DEFAULT ON)
endif()

option(USE_NATIVE_INSTRUCTIONS "-march=native" ON)
option(WITH_LLVM "Enable usage of LLVM library" ON)
option(BUILD_LLVM "Build LLVM from git submodule" OFF)
option(STATIC_LINK_LLVM "Link against LLVM statically." OFF)
option(USE_FAUDIO "FAudio audio backend" ON)
option(USE_LIBEVDEV "libevdev-based joystick support" ON)
option(USE_DISCORD_RPC "Discord rich presence integration" OFF)
option(USE_VULKAN "Vulkan render backend" ON)
option(USE_PRECOMPILED_HEADERS "Use precompiled headers" OFF)
option(USE_SDL "Enables SDL input handler" ON)
option(USE_SYSTEM_CUBEB "Prefer system cubeb" OFF)
option(USE_SYSTEM_CURL "Prefer system Curl" ON)
option(USE_SYSTEM_FAUDIO "Prefer system FAudio" OFF)
option(USE_SYSTEM_FFMPEG "Prefer system ffmpeg" OFF)
option(USE_SYSTEM_PROTOBUF "Prefer system protobuf" OFF)
option(USE_SYSTEM_GLSLANG "Prefer system glslang" OFF)
option(USE_SYSTEM_HIDAPI "Prefer system hidapi" OFF)
option(USE_SYSTEM_LIBPNG "Prefer system libpng" OFF)
option(USE_SYSTEM_LIBUSB "Prefer system libusb" OFF)
option(USE_SYSTEM_MINIUPNPC "Prefer system MiniUPnPc" OFF)
option(USE_SYSTEM_MVK "Prefer system MoltenVK" OFF)
option(USE_SYSTEM_OPENAL "Prefer system OpenAL" ${USE_SYSTEM_OPENAL_DEFAULT})
option(USE_SYSTEM_OPENCV "Prefer system OpenCV" ON)
option(USE_SYSTEM_PUGIXML "Prefer system pugixml" OFF)
option(USE_SYSTEM_RTMIDI "Prefer system RtMidi" OFF)
option(USE_SYSTEM_SDL "Prefer system SDL" ON)
option(USE_SYSTEM_VULKAN_MEMORY_ALLOCATOR "Prefer system VMA" OFF)
option(USE_SYSTEM_WOLFSSL "Prefer system wolfSSL" OFF)
option(USE_SYSTEM_ZLIB "Prefer system ZLIB" ON)
option(USE_SYSTEM_ZSTD "Prefer system zstd" OFF)
option(HAS_MEMORY_BREAKPOINTS "Memory breakpoints in the interpreter" OFF)
option(USE_LTO "Use LTO for building" ON)
option(BUILD_RPCS3_TESTS "Build RPCS3 unit tests." OFF)
option(RUN_RPCS3_TESTS "Run RPCS3 unit tests." OFF)
option(USE_GAMEMODE "Enable GameMode features." ON)

set(CMAKE_MODULE_PATH "${RPCS3_DIR}/buildfiles/cmake")
include(CheckCXXCompilerFlag)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo")
endif()
if(CMAKE_BUILD_TYPE MATCHES "Debug")
    add_compile_definitions(_DEBUG)
endif()

# rpcs3's arm64 macOS builds rely on the Homebrew prefix being searched.
if(CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew")
    list(APPEND CMAKE_SYSTEM_PREFIX_PATH "/opt/homebrew")
endif()

find_package(Threads REQUIRED)

# ---- 3rdparty (built with its OWN flags, before ConfigureCompiler) ----------
add_subdirectory("${RPCS3_DIR}/3rdparty" "${CMAKE_BINARY_DIR}/3rdparty")

# openal-soft bundles fmt-11.2.0, but a Homebrew-found backend puts
# -I/opt/homebrew/include (which contains a different fmt) on the OpenAL target
# ahead of the bundled one, so <fmt/base.h> resolves to Homebrew's fmt and
# breaks hrtf_loader.cpp. Prepend the bundled fmt so it wins.
foreach(_al_tgt OpenAL alsoft.common alsoft.excommon)
    if(TARGET ${_al_tgt})
        target_include_directories(${_al_tgt} BEFORE PRIVATE
            "${RPCS3_DIR}/3rdparty/OpenAL/openal-soft/fmt-11.2.0/include")
    endif()
endforeach()

# ---- core compiler flags (scoped to Emu + bridge, NOT 3rdparty) -------------
# rpcs3 applies these in its app-level CMakeLists, after 3rdparty; do the same
# so 3rdparty isn't subjected to -Werror.
include(ConfigureCompiler)
include(CheckFunctionExists)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Homebrew clang uses its own libc++ headers (which declare newer symbols like
# std::__hash_memory), so link against its libc++ runtime rather than the older
# one in the macOS SDK, or those symbols are undefined at link time.
get_filename_component(_llvm_bindir "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(LLVM_CXX_LIBDIR "${_llvm_bindir}/../lib/c++" ABSOLUTE)
if(EXISTS "${LLVM_CXX_LIBDIR}/libc++.dylib")
    add_link_options("-L${LLVM_CXX_LIBDIR}" "-Wl,-rpath,${LLVM_CXX_LIBDIR}")
endif()

# Generate rpcs3/rpcs3/git-version.h (rpcs3_version.cpp includes it).
include("${RPCS3_DIR}/rpcs3/git-version.cmake")
gen_git_version("${RPCS3_DIR}/rpcs3")

# ---- the core (rpcs3_emu) ---------------------------------------------------
add_subdirectory("${RPCS3_DIR}/rpcs3/Emu" "${CMAKE_BINARY_DIR}/Emu")

# Preserve the include root the standalone build gives rpcs3_emu (patch 0001
# swapped CMAKE_SOURCE_DIR -> PROJECT_SOURCE_DIR, which under this superproject
# would point at macos/ rather than the rpcs3 tree).
target_include_directories(rpcs3_emu PUBLIC "${RPCS3_DIR}")

# System opencv/ffmpeg/curl pull -I/opt/homebrew/include onto rpcs3_emu, where
# Homebrew's newer protobuf headers shadow the vendored ones — but the checked-in
# *.pb.h gencode requires (and rpcs3_emu links) the vendored protobuf runtime, so
# the version check hard-fails. Prepend the vendored protobuf src so it wins.
target_include_directories(rpcs3_emu BEFORE PRIVATE
    "${RPCS3_DIR}/3rdparty/protobuf/protobuf/src")

# ---- our bridge + harness ---------------------------------------------------
add_subdirectory(bridge)
