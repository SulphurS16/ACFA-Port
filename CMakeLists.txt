cmake_minimum_required(VERSION 3.25)
project(acfa LANGUAGES C CXX)
if(WIN32)
  enable_language(RC)
endif()
set(ACFA_GAME_PROFILE "us-blus30187" CACHE STRING "Exact game executable profile")
set_property(CACHE ACFA_GAME_PROFILE PROPERTY STRINGS jp-bljm55005 us-blus30187)
if(ACFA_GAME_PROFILE STREQUAL "jp-bljm55005")
  add_compile_definitions(ACFA_GAME_JP=1)
elseif(ACFA_GAME_PROFILE STREQUAL "us-blus30187")
  add_compile_definitions(ACFA_GAME_JP=0)
else()
  message(FATAL_ERROR "Unrecognized game profile: ${ACFA_GAME_PROFILE}")
endif()
include_directories(${CMAKE_SOURCE_DIR}/third_party/simde)
set(ACFA_CONTROLLER_ONLY_DEFAULT OFF)
if(APPLE OR ACFA_GAME_PROFILE STREQUAL "jp-bljm55005")
  set(ACFA_CONTROLLER_ONLY_DEFAULT ON)
endif()
option(ACFA_CONTROLLER_ONLY "Disable all keyboard/mouse gameplay and camera hooks" ${ACFA_CONTROLLER_ONLY_DEFAULT})
option(ACFA_BUILD_RUNTIME "Build the runtime (Mac requires separate experimental opt-in)" ${WIN32})
option(ACFA_EXPERIMENTAL_MAC_RUNTIME "Link the experimental native Mac runtime" OFF)
if(ACFA_BUILD_RUNTIME AND NOT WIN32)
  if(NOT APPLE OR NOT ACFA_EXPERIMENTAL_MAC_RUNTIME)
    message(FATAL_ERROR "Mac runtime is experimental. Tools use ACFA_BUILD_RUNTIME=OFF; developers must explicitly opt in with ACFA_EXPERIMENTAL_MAC_RUNTIME=ON.")
  endif()
  if(NOT ACFA_CONTROLLER_ONLY)
    message(FATAL_ERROR "The native Mac runtime supports controllers only")
  endif()
  message(WARNING "Experimental Mac runtime: offscreen tests do not establish game compatibility")
endif()
add_compile_definitions(ACFA_CONTROLLER_ONLY=$<BOOL:${ACFA_CONTROLLER_ONLY}>)
if(ACFA_GAME_PROFILE STREQUAL "jp-bljm55005" AND NOT ACFA_CONTROLLER_ONLY)
  message(FATAL_ERROR "JP keyboard/mouse camera hooks are intentionally unsupported")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(WIN32)
  add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX UNICODE _UNICODE)
endif()

# The port's version (VERSION; the release's changelog names it), shown by the launcher and logged
# by the game.
file(STRINGS ${CMAKE_SOURCE_DIR}/VERSION ACFA_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/VERSION)
add_compile_definitions(ACFA_VERSION="${ACFA_VERSION}")

# Developer diagnostics (control-file commands, profiler, memory search and watchpoints, traces,
# frame reports, A/B switches, test autopilot). Off in player builds: compiled out, and the
# release export (tools\release) removes their code.
option(ACFA_DEV "Build with developer diagnostics" OFF)
if(ACFA_DEV)
  add_compile_definitions(ACFA_DEV=1)
else()
  add_compile_definitions(ACFA_DEV=0)
endif()

# Self-contained executables (no libc++/libunwind DLLs next to them), reproducible: no build
# paths or link timestamps in the binaries.
if(MINGW)
  add_link_options(-static -Wl,--no-insert-timestamp)
endif()
add_compile_options(-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.)
# UTF-8 process code page for every executable (paths with any characters).
if(WIN32)
  set(ACFA_UTF8_RC ${CMAKE_SOURCE_DIR}/src/common/utf8.rc)
endif()

# ---- third party ----
file(GLOB ZLIB_SRC ${CMAKE_SOURCE_DIR}/third_party/zlib/*.c)
add_library(acfa_zlib STATIC ${ZLIB_SRC})
target_include_directories(acfa_zlib PUBLIC ${CMAKE_SOURCE_DIR}/third_party/zlib)
target_compile_options(acfa_zlib PRIVATE -w)

# ---- common ----
add_library(acfa_common STATIC
  src/common/bytes.cpp
  src/common/crypto.cpp
  src/common/known_inputs.cpp
  src/common/file.cpp
  src/common/ini.cpp
  src/common/privacy.cpp
  src/common/settings_file.cpp
  src/common/sfo.cpp)
target_include_directories(acfa_common PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(acfa_common PUBLIC acfa_zlib)
if(WIN32)
  target_link_libraries(acfa_common PUBLIC bcrypt)
elseif(NOT APPLE)
  message(FATAL_ERROR "This branch currently supports Apple and Windows host tools")
endif()
target_compile_options(acfa_common PRIVATE -Wall -Wextra)

# ---- PPU decoding (shared by recompiler and runtime) ----
add_library(acfa_ppu STATIC src/ppu/ppu_decode.cpp)
target_link_libraries(acfa_ppu PUBLIC acfa_common)
target_compile_options(acfa_ppu PRIVATE -Wall -Wextra)

# ---- SPU ISA, chunk discovery (shared by the SPU recompiler and the runtime) ----
add_library(acfa_spu STATIC src/spu/spu_isa.cpp)
target_link_libraries(acfa_spu PUBLIC acfa_common)
target_compile_options(acfa_spu PRIVATE -Wall -Wextra)

add_executable(acfa-spurecomp src/spurecomp/main.cpp ${ACFA_UTF8_RC})
target_link_libraries(acfa-spurecomp PRIVATE acfa_spu)
target_compile_options(acfa-spurecomp PRIVATE -Wall -Wextra)

# ---- recompiler ----
add_executable(acfa-recomp
  src/recomp/main.cpp
  src/recomp/image.cpp
  src/recomp/analysis.cpp
  src/recomp/emit.cpp
  ${ACFA_UTF8_RC})
target_link_libraries(acfa-recomp PRIVATE acfa_ppu)
target_compile_options(acfa-recomp PRIVATE -Wall -Wextra)

# ---- generated PPU code (game-derived, from local/gen/ppu; never distributed) ----
set(ACFA_GEN_DIR "${CMAKE_SOURCE_DIR}/local/gen/ppu" CACHE PATH "Directory with acfa-recomp output")
file(GLOB ACFA_GEN_SRC CONFIGURE_DEPENDS "${ACFA_GEN_DIR}/*.cpp")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  set(ACFA_CPU_FLAGS -march=haswell)
else()
  set(ACFA_CPU_FLAGS "")
endif()

if(ACFA_BUILD_RUNTIME AND NOT ACFA_GEN_SRC)
  message(FATAL_ERROR "Runtime requires the user's prepared PPU sources: set ACFA_GEN_DIR")
endif()
if(ACFA_GEN_SRC)
  add_library(acfa_ppu_gen STATIC ${ACFA_GEN_SRC})
  target_include_directories(acfa_ppu_gen PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(acfa_ppu_gen PRIVATE -O2 ${ACFA_CPU_FLAGS} -fno-strict-aliasing -w)
  target_precompile_headers(acfa_ppu_gen PRIVATE ${CMAKE_SOURCE_DIR}/src/ppu/ppu_recomp.h)
endif()

# Recompiled SPU code is independently buildable on ARM64, like generated PPU code.
# Generated sources stay local; no fallback object is needed for tool-only builds.
set(ACFA_SPU_GEN_DIR "${CMAKE_SOURCE_DIR}/local/gen/spu" CACHE PATH "Directory with acfa-spurecomp output")
file(GLOB ACFA_SPU_GEN_SRC CONFIGURE_DEPENDS "${ACFA_SPU_GEN_DIR}/*.cpp")
if(ACFA_SPU_GEN_SRC OR ACFA_BUILD_RUNTIME)
  if(NOT ACFA_SPU_GEN_SRC)
    set(ACFA_SPU_GEN_SRC ${CMAKE_SOURCE_DIR}/src/spu/spu_gen_empty.cpp)
  endif()
  add_library(acfa_spu_gen STATIC ${ACFA_SPU_GEN_SRC})
  target_include_directories(acfa_spu_gen PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(acfa_spu_gen PRIVATE -O2 ${ACFA_CPU_FLAGS} -fno-strict-aliasing -w)
endif()

if(ACFA_GEN_SRC AND ACFA_BUILD_RUNTIME)
  file(GLOB ACFA_RT_SRC CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/runtime/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/hle/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/lv2/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/rsx/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/gpu/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/audio/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/media/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/game/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/spu/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/ui/*.cpp)
  if(NOT APPLE)
    list(REMOVE_ITEM ACFA_RT_SRC ${CMAKE_SOURCE_DIR}/src/runtime/audio/audio_out_macos.cpp)
  else()
    list(REMOVE_ITEM ACFA_RT_SRC ${CMAKE_SOURCE_DIR}/src/runtime/audio/audio_out.cpp)
  endif()
  if(ACFA_CONTROLLER_ONLY)
    list(REMOVE_ITEM ACFA_RT_SRC
      ${CMAKE_SOURCE_DIR}/src/runtime/input.cpp
      ${CMAKE_SOURCE_DIR}/src/runtime/game/input_hooks.cpp
      ${CMAKE_SOURCE_DIR}/src/runtime/game/menu_mouse.cpp)
  else()
    list(REMOVE_ITEM ACFA_RT_SRC ${CMAKE_SOURCE_DIR}/src/runtime/input_controller_only.cpp
      ${CMAKE_SOURCE_DIR}/src/runtime/game/controller_only.cpp)
  endif()
  if(APPLE)
    # The original RSX translator/renderer lives in acfa_mac_gpu. Native backends
    # replace Win32 input/window/device/display; no keyboard/mouse implementation is linked.
    list(FILTER ACFA_RT_SRC EXCLUDE REGEX "/gpu/")
    list(REMOVE_ITEM ACFA_RT_SRC ${CMAKE_SOURCE_DIR}/src/runtime/bindings.cpp
      ${CMAKE_SOURCE_DIR}/src/runtime/gamepads.cpp ${CMAKE_SOURCE_DIR}/src/runtime/audio/audio_out_macos.cpp)
    add_executable(acfa ${ACFA_RT_SRC} src/platform/macos/guest_memory.cpp)
  else()
    list(REMOVE_ITEM ACFA_RT_SRC ${CMAKE_SOURCE_DIR}/src/runtime/bindings_controller_only.cpp)
    add_executable(acfa ${ACFA_RT_SRC} ${ACFA_UTF8_RC} src/runtime/game.rc)
  endif()
  set(ACFA_RUNTIME_OUTPUT_NAME "acfa" CACHE STRING "Runtime executable filename (separate candidates keep a running build untouched)")
  set_target_properties(acfa PROPERTIES OUTPUT_NAME "${ACFA_RUNTIME_OUTPUT_NAME}")
  if(NOT ACFA_DEV)  # players' builds open no console window (the run log holds every message)
    set_target_properties(acfa PROPERTIES WIN32_EXECUTABLE ON)
  endif()
  target_include_directories(acfa PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(acfa PRIVATE ${ACFA_CPU_FLAGS} -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter -g)
  # Whole-archive: the function table references everything anyway; keeps HLE registrars alive.
  if(APPLE)
    target_link_libraries(acfa PRIVATE acfa_ppu_gen acfa_spu_gen acfa_spu acfa_ppu acfa_common
      acfa_mac_gpu acfa_mac_window acfa_mac_gamepads acfa_mac_host acfa_audio_host
      "-framework CoreFoundation" "-framework CoreGraphics" "-framework CoreText")
  else()
    target_link_libraries(acfa PRIVATE acfa_ppu_gen acfa_spu_gen acfa_spu acfa_ppu acfa_common d3d11 dxgi d3dcompiler d2d1 dwrite ole32 psapi
                          synchronization hid setupapi)
  endif()
endif()

# ---- preparation tool ----
add_executable(acfa-prep
  src/prep/main.cpp
  src/prep/import.cpp
  src/prep/prx.cpp
  src/prep/sfo.cpp
  src/prep/spu_records.cpp
  ${ACFA_UTF8_RC})
target_link_libraries(acfa-prep PRIVATE acfa_common)
target_compile_options(acfa-prep PRIVATE -Wall -Wextra)

# ---- launcher (players' setup and start; no game data or game-derived code) ----
if(WIN32)
add_executable(acfa-launcher WIN32
  src/launcher/main.cpp
  src/launcher/setup.cpp
  src/launcher/inputs.cpp
  src/launcher/process.cpp
  src/launcher/rpcs3.cpp
  src/launcher/download.cpp
  src/launcher/source.cpp
  src/launcher/install.cpp
  src/launcher/launcher.rc)
# The launcher's file version (its properties): the numbers of VERSION, for its resource file only.
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" acfa_version_numbers "${ACFA_VERSION}")
set_source_files_properties(src/launcher/launcher.rc PROPERTIES COMPILE_DEFINITIONS
  "ACFA_VERSION_MAJOR=${CMAKE_MATCH_1};ACFA_VERSION_MINOR=${CMAKE_MATCH_2};ACFA_VERSION_PATCH=${CMAKE_MATCH_3}")
# A release launcher carries the port's sources (a zip of the exported tree, made by
# tools/release/export.py) and unpacks them into its install folder (src/launcher/source.h).
set(ACFA_SOURCE_ZIP "" CACHE FILEPATH "Zip of the port's sources to embed in the launcher (releases)")
if(ACFA_SOURCE_ZIP)
  file(TO_CMAKE_PATH "${ACFA_SOURCE_ZIP}" acfa_source_zip)
  file(WRITE ${CMAKE_BINARY_DIR}/launcher_source.rc "2 RCDATA \"${acfa_source_zip}\"\n")
  target_sources(acfa-launcher PRIVATE ${CMAKE_BINARY_DIR}/launcher_source.rc)
  set_source_files_properties(${CMAKE_BINARY_DIR}/launcher_source.rc PROPERTIES OBJECT_DEPENDS "${acfa_source_zip}")
endif()
target_include_directories(acfa-launcher PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(acfa-launcher PRIVATE acfa_common comctl32 winhttp shell32 ole32 uuid advapi32)
target_link_options(acfa-launcher PRIVATE -municode)
target_compile_options(acfa-launcher PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
set_target_properties(acfa-launcher PROPERTIES OUTPUT_NAME ACFA-Launcher)
endif()

# ---- media decoder test tool (development: checks the MPEG-2 decoder on the user's movies) ----

include(CTest)
if(BUILD_TESTING AND APPLE AND ACFA_CONTROLLER_ONLY)
  add_executable(acfa-native-host-test tests/native_host.cpp src/runtime/input_controller_only.cpp)
  target_link_libraries(acfa-native-host-test PRIVATE acfa_common)
  target_compile_options(acfa-native-host-test PRIVATE -fno-strict-aliasing)
  add_test(NAME native-host COMMAND acfa-native-host-test ${CMAKE_BINARY_DIR}/test-scratch)
endif()

if(APPLE)
  enable_language(OBJCXX)
  # The Metal submit worker needs a pool around each executed command chunk.
  set_source_files_properties(src/runtime/gpu/submit.cpp PROPERTIES LANGUAGE OBJCXX COMPILE_OPTIONS "-fobjc-arc")
  add_library(acfa_mac_gamepads STATIC src/platform/macos/gamepads.mm)
  target_include_directories(acfa_mac_gamepads PUBLIC ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(acfa_mac_gamepads PRIVATE -fobjc-arc)
  target_link_libraries(acfa_mac_gamepads PUBLIC "-framework Foundation" "-framework GameController")
  add_executable(acfa-pad-probe tools/pad_probe.mm)
  target_link_libraries(acfa-pad-probe PRIVATE acfa_mac_gamepads)
endif()

if(APPLE AND BUILD_TESTING)
  add_executable(acfa-native-vm-test tests/native_vm.cpp src/runtime/memory.cpp src/platform/macos/guest_memory.cpp)
  target_include_directories(acfa-native-vm-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(acfa-native-vm-test PRIVATE acfa_common)
  add_test(NAME native-vm COMMAND acfa-native-vm-test)
endif()

# Native Metal shader bridge: isolated Slang SDK, no system installation.
option(ACFA_REQUIRE_METAL_BRIDGE "Require the isolated Slang SDK for Metal validation" OFF)
if(APPLE)
  set(ACFA_SLANG_ROOT "${CMAKE_SOURCE_DIR}/../local/deps/slang-2026.20" CACHE PATH "Slang ARM64 SDK root")
  find_path(ACFA_SLANG_INCLUDE_DIR slang.h HINTS "${ACFA_SLANG_ROOT}/include" NO_DEFAULT_PATH)
  find_library(ACFA_SLANG_LIBRARY NAMES slang-compiler HINTS "${ACFA_SLANG_ROOT}/lib" NO_DEFAULT_PATH)
  if(ACFA_SLANG_INCLUDE_DIR AND ACFA_SLANG_LIBRARY)
    message(STATUS "Native Metal shader bridge: ${ACFA_SLANG_LIBRARY}")
    add_library(acfa_metal_shader STATIC src/platform/macos/metal_shader.cpp)
    target_include_directories(acfa_metal_shader PUBLIC ${CMAKE_SOURCE_DIR}/src PRIVATE ${ACFA_SLANG_INCLUDE_DIR})
    target_link_libraries(acfa_metal_shader PUBLIC ${ACFA_SLANG_LIBRARY})
    add_executable(acfa-shader-probe tools/shader_probe.cpp src/runtime/gpu/vp_hlsl.cpp src/runtime/gpu/fp_hlsl.cpp)
    target_include_directories(acfa-shader-probe PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    target_link_libraries(acfa-shader-probe PRIVATE acfa_metal_shader acfa_common)
    if(BUILD_TESTING)
      add_executable(acfa-native-metal-test tests/native_metal.mm src/runtime/gpu/vp_hlsl.cpp src/runtime/gpu/fp_hlsl.cpp)
      target_include_directories(acfa-native-metal-test PRIVATE ${CMAKE_SOURCE_DIR}/tests)
      target_compile_options(acfa-native-metal-test PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
      target_link_libraries(acfa-native-metal-test PRIVATE acfa_metal_shader acfa_common "-framework Foundation" "-framework Metal")
      add_test(NAME native-metal COMMAND acfa-native-metal-test ${CMAKE_SOURCE_DIR})
      set_tests_properties(native-metal PROPERTIES TIMEOUT 90)
    endif()
  elseif(ACFA_REQUIRE_METAL_BRIDGE)
    message(FATAL_ERROR "Slang SDK missing. Run python3 tools/fetch-slang-macos.py, then reconfigure.")
  else()
    message(STATUS "Metal validation disabled: Slang SDK missing (tools/fetch-slang-macos.py).")
  endif()
endif()

if(APPLE)
  add_library(acfa_audio_host STATIC src/runtime/audio/audio_out_macos.cpp)
  target_include_directories(acfa_audio_host PUBLIC ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(acfa_audio_host PUBLIC acfa_common "-framework AudioUnit" "-framework AudioToolbox" "-framework CoreAudio")
  if(BUILD_TESTING)
    add_executable(acfa-native-audio-test tests/native_audio.cpp)
    target_link_libraries(acfa-native-audio-test PRIVATE acfa_audio_host)
    add_test(NAME native-audio COMMAND acfa-native-audio-test)
    set_tests_properties(native-audio PROPERTIES TIMEOUT 30)
  endif()
endif()

if(APPLE)
  add_library(acfa_mac_host STATIC src/platform/macos/host_thread.cpp)
  target_include_directories(acfa_mac_host PUBLIC ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(acfa_mac_host PRIVATE -Wall -Wextra -Werror)
  add_library(acfa_mac_runtime_core OBJECT src/runtime/ppu_runtime.cpp src/runtime/log.cpp)
  target_link_libraries(acfa_mac_runtime_core PRIVATE acfa_common acfa_mac_host)
  target_compile_options(acfa_mac_runtime_core PRIVATE -Wall -Wextra -fno-strict-aliasing)
  if(BUILD_TESTING)
    add_executable(acfa-native-threads-test tests/native_threads.cpp)
    target_link_libraries(acfa-native-threads-test PRIVATE acfa_mac_host)
    add_test(NAME native-threads COMMAND acfa-native-threads-test)
    set_tests_properties(native-threads PROPERTIES TIMEOUT 30)
  endif()
endif()

if(APPLE AND BUILD_TESTING)
  add_executable(acfa-native-runtime-core-test tests/native_runtime_core.cpp
    $<TARGET_OBJECTS:acfa_mac_runtime_core> src/runtime/memory.cpp src/platform/macos/guest_memory.cpp
    src/runtime/game/texture_readback.cpp)
  target_link_libraries(acfa-native-runtime-core-test PRIVATE acfa_common acfa_mac_host)
  target_compile_options(acfa-native-runtime-core-test PRIVATE -fno-strict-aliasing)
  add_test(NAME native-runtime-core COMMAND acfa-native-runtime-core-test)
  set_tests_properties(native-runtime-core PROPERTIES TIMEOUT 30)
endif()

# Compile the real nongraphics runtime separately until the Metal RSX renderer is integrated.
# This object target is deliberately not a playable executable and does not link game code.
if(APPLE)
  file(GLOB_RECURSE ACFA_MAC_CHECK_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/runtime/*.cpp)
  list(FILTER ACFA_MAC_CHECK_SRC EXCLUDE REGEX "/(gpu|ui)/")
  list(FILTER ACFA_MAC_CHECK_SRC EXCLUDE REGEX "/(main|bindings|gamepads|input|sysdialog|audio_out|input_hooks|menu_mouse)\\.cpp$")
  add_library(acfa_mac_runtime_check OBJECT ${ACFA_MAC_CHECK_SRC})
  target_link_libraries(acfa_mac_runtime_check PRIVATE acfa_common acfa_mac_host "-framework CoreFoundation")
  target_compile_options(acfa_mac_runtime_check PRIVATE -fno-strict-aliasing)
endif()

if(APPLE)
  add_library(acfa_mac_window STATIC src/platform/macos/window_macos.mm)
  target_include_directories(acfa_mac_window PUBLIC ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(acfa_mac_window PRIVATE -fobjc-arc -Wall -Wextra -Werror)
  target_link_libraries(acfa_mac_window PUBLIC acfa_common "-framework Cocoa" "-framework QuartzCore" "-framework Metal")
  if(BUILD_TESTING)
    add_executable(acfa-native-vfs tests/native_vfs.cpp src/runtime/vfs.cpp)
    target_link_libraries(acfa-native-vfs PRIVATE acfa_common)
    add_test(NAME native-vfs COMMAND acfa-native-vfs ${CMAKE_BINARY_DIR}/vfs-test-scratch)
    set_tests_properties(native-vfs PROPERTIES TIMEOUT 30)
  endif()
endif()

# Native Metal implementation of the renderer's internal call vocabulary.
if(APPLE AND TARGET acfa_metal_shader)
  add_library(acfa_metal_api STATIC src/platform/macos/metal_resources.mm src/platform/macos/metal_context.mm)
  target_include_directories(acfa_metal_api PUBLIC ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(acfa_metal_api PRIVATE -fobjc-arc -Wall -Wextra)
  target_link_libraries(acfa_metal_api PUBLIC acfa_common "-framework Foundation" "-framework Metal" "-framework QuartzCore")
  add_library(acfa_mac_gpu STATIC
    src/platform/macos/device_macos.cpp src/platform/macos/display_macos.cpp
    src/platform/macos/shader_prewarm.cpp
    src/runtime/gpu/renderer.cpp src/runtime/gpu/submit.cpp src/runtime/gpu/rsx_texture.cpp
    src/runtime/gpu/vp_hlsl.cpp src/runtime/gpu/fp_hlsl.cpp src/runtime/gpu/shader_warm.cpp src/runtime/gpu/png.cpp)
  target_include_directories(acfa_mac_gpu PUBLIC ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(acfa_mac_gpu PRIVATE -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(acfa_mac_gpu PUBLIC acfa_metal_api acfa_metal_shader acfa_mac_host acfa_common "-framework CoreGraphics")
  if(BUILD_TESTING)
    add_executable(acfa-native-metal-resources tests/native_metal_resources.mm)
    target_compile_options(acfa-native-metal-resources PRIVATE -fobjc-arc)
    target_link_libraries(acfa-native-metal-resources PRIVATE acfa_metal_api)
    add_test(NAME native-metal-resources COMMAND acfa-native-metal-resources)
    add_executable(acfa-native-metal-context tests/native_metal_context.mm)
    target_compile_options(acfa-native-metal-context PRIVATE -fobjc-arc)
    target_link_libraries(acfa-native-metal-context PRIVATE acfa_metal_api)
    add_test(NAME native-metal-context COMMAND acfa-native-metal-context)
    set_tests_properties(native-metal-resources native-metal-context PROPERTIES TIMEOUT 45)
  endif()
endif()
if(APPLE AND BUILD_TESTING)
  add_executable(acfa-native-display tests/native_display.cpp src/platform/macos/display_macos.cpp)
  target_link_libraries(acfa-native-display PRIVATE acfa_mac_host acfa_common "-framework CoreGraphics" "-framework CoreFoundation")
  add_test(NAME native-display COMMAND acfa-native-display)
  add_executable(acfa-native-sysdialog tests/native_sysdialog.cpp src/runtime/sysdialog.cpp)
  target_link_libraries(acfa-native-sysdialog PRIVATE acfa_common "-framework CoreFoundation")
  add_test(NAME native-sysdialog COMMAND acfa-native-sysdialog)
  set_tests_properties(native-display native-sysdialog PROPERTIES TIMEOUT 15)
endif()

if(APPLE AND BUILD_TESTING AND TARGET acfa_metal_api)
  # The fixture includes the actual overlay implementation to inspect its model.
  add_executable(acfa-native-overlay tests/native_overlay.cpp src/runtime/sysdialog.cpp src/runtime/bindings_controller_only.cpp)
  target_link_libraries(acfa-native-overlay PRIVATE acfa_metal_api acfa_common
    "-framework CoreGraphics" "-framework CoreText" "-framework ImageIO")
  add_test(NAME native-overlay COMMAND acfa-native-overlay)
  set_tests_properties(native-overlay PROPERTIES TIMEOUT 45)
endif()

if(APPLE AND ACFA_BUILD_RUNTIME AND NOT TARGET acfa_mac_gpu)
  message(FATAL_ERROR "Native runtime requires the isolated Slang SDK (ACFA_SLANG_ROOT)")
endif()

if(APPLE AND BUILD_TESTING AND TARGET acfa_mac_gpu)
  add_executable(acfa-native-rsx tests/native_rsx.cpp src/runtime/memory.cpp src/platform/macos/guest_memory.cpp)
  target_include_directories(acfa-native-rsx PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_compile_options(acfa-native-rsx PRIVATE -fno-strict-aliasing)
  target_link_libraries(acfa-native-rsx PRIVATE acfa_mac_gpu)
  add_test(NAME native-rsx COMMAND acfa-native-rsx)
  set_tests_properties(native-rsx PROPERTIES TIMEOUT 45)
endif()

if(APPLE AND BUILD_TESTING AND TARGET acfa_metal_api)
  add_executable(acfa-native-metal-autorelease tests/native_metal_autorelease.mm)
  target_compile_options(acfa-native-metal-autorelease PRIVATE -fobjc-arc)
  target_link_libraries(acfa-native-metal-autorelease PRIVATE acfa_metal_api)
  add_test(NAME native-metal-autorelease COMMAND acfa-native-metal-autorelease)
  set_tests_properties(native-metal-autorelease PROPERTIES TIMEOUT 45)
endif()

if(APPLE AND BUILD_TESTING AND TARGET acfa_metal_api)
  add_executable(acfa-native-metal-shader-cache tests/native_metal_shader_cache.mm)
  target_compile_options(acfa-native-metal-shader-cache PRIVATE -fobjc-arc)
  target_link_libraries(acfa-native-metal-shader-cache PRIVATE acfa_metal_api)
  add_test(NAME native-metal-shader-cache COMMAND acfa-native-metal-shader-cache)
  set_tests_properties(native-metal-shader-cache PROPERTIES TIMEOUT 90)
  add_executable(acfa-native-shader-prewarm tests/native_shader_prewarm.cpp src/platform/macos/shader_prewarm.cpp)
  target_link_libraries(acfa-native-shader-prewarm PRIVATE acfa_common)
  add_test(NAME native-shader-prewarm COMMAND acfa-native-shader-prewarm)
  set_tests_properties(native-shader-prewarm PROPERTIES TIMEOUT 15)
endif()

if(APPLE AND BUILD_TESTING AND TARGET acfa_metal_api)
  add_executable(acfa-native-shader-prewarm-gpu tests/native_shader_prewarm_gpu.mm src/platform/macos/shader_prewarm.cpp)
  target_compile_options(acfa-native-shader-prewarm-gpu PRIVATE -fobjc-arc)
  target_link_libraries(acfa-native-shader-prewarm-gpu PRIVATE acfa_metal_api)
endif()

if(APPLE AND BUILD_TESTING AND TARGET acfa_metal_api)
  add_executable(acfa-native-metal-batch-benchmark tests/native_metal_batch_benchmark.mm)
  target_compile_options(acfa-native-metal-batch-benchmark PRIVATE -fobjc-arc)
  target_link_libraries(acfa-native-metal-batch-benchmark PRIVATE acfa_metal_api)
endif()
