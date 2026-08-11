# cellstation_bridge — a C-ABI layer over the unmodified rpcs3_emu core, and a
# small CLI harness that drives it headless. This is the macOS analogue of
# native/bridge (JNI), with the JNI/Android specifics removed.
#
# The upstream Input/ pad sources are compiled here (not by rpcs3qt, which we do
# not build) because pad_thread — referenced by our init_pad_handler callback —
# lives there. Mirrors native/bridge/CMakeLists.txt's source list.

set(RPCS3_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../rpcs3/rpcs3")

add_library(cellstation_bridge STATIC
    cellstation_bridge.cpp
    core_compat_macos.cpp
    "${RPCS3_SRC_DIR}/rpcs3_version.cpp"
    "${RPCS3_SRC_DIR}/Input/ds3_pad_handler.cpp"
    "${RPCS3_SRC_DIR}/Input/ds4_pad_handler.cpp"
    "${RPCS3_SRC_DIR}/Input/dualsense_pad_handler.cpp"
    "${RPCS3_SRC_DIR}/Input/hid_pad_handler.cpp"
    "${RPCS3_SRC_DIR}/Input/pad_thread.cpp"
    "${RPCS3_SRC_DIR}/Input/product_info.cpp"
    "${RPCS3_SRC_DIR}/Input/ps_move_calibration.cpp"
    "${RPCS3_SRC_DIR}/Input/ps_move_config.cpp"
    "${RPCS3_SRC_DIR}/Input/ps_move_handler.cpp"
    "${RPCS3_SRC_DIR}/Input/ps_move_tracker.cpp"
    "${RPCS3_SRC_DIR}/Input/skateboard_pad_handler.cpp"
)

target_include_directories(cellstation_bridge PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# pad_thread.cpp's keyboard handler is Qt (keyboard_pad_handler -> QWindow); we
# don't link Qt. macos/patches/0001 gates it behind this define so the keyboard
# pad falls back to the Null handler (the same path Android takes).
target_compile_definitions(cellstation_bridge PRIVATE CELLSTATION_NO_QT_KEYBOARD)

# rpcs3_emu propagates its public include roots (stdafx, 3rdparty) and its
# 3rdparty link interface transitively.
target_link_libraries(cellstation_bridge PUBLIC
    rpcs3_emu
    3rdparty::hidapi
    3rdparty::fusion
)

# System libs/frameworks the rpcs3 desktop frontend (rpcs3_lib) normally links
# but which we must provide ourselves since we don't build it: iconv (StrFmt),
# and Foundation (sysinfo_darwin.mm -> NSProcessInfo). Others (CoreAudio via
# cubeb, IOKit via hidapi, CoreMIDI via rtmidi, OpenAL) arrive through the
# 3rdparty link interface.
find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
target_link_libraries(cellstation_bridge PUBLIC
    iconv
    ${FOUNDATION_FRAMEWORK}
    ${IOKIT_FRAMEWORK}
)

# ---- CLI harness (M2): boot a payload headless, drive it over a stdin REPL ---
add_executable(cellstation_harness "${CMAKE_CURRENT_SOURCE_DIR}/../harness/main.cpp")
target_link_libraries(cellstation_harness PRIVATE cellstation_bridge)
