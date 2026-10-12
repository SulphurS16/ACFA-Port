cmake_minimum_required(VERSION 3.25)
project(acfa LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX UNICODE _UNICODE)

# Self-contained executables (no libc++/libunwind DLLs next to them).
if(MINGW)
  add_link_options(-static)
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
  src/common/file.cpp
  src/common/sfo.cpp)
target_include_directories(acfa_common PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(acfa_common PUBLIC acfa_zlib bcrypt)
target_compile_options(acfa_common PRIVATE -Wall -Wextra)

# ---- PPU decoding (shared by recompiler and runtime) ----
add_library(acfa_ppu STATIC src/ppu/ppu_decode.cpp)
target_link_libraries(acfa_ppu PUBLIC acfa_common)
target_compile_options(acfa_ppu PRIVATE -Wall -Wextra)

# ---- SPU ISA, chunk discovery (shared by the SPU recompiler and the runtime) ----
add_library(acfa_spu STATIC src/spu/spu_isa.cpp)
target_link_libraries(acfa_spu PUBLIC acfa_common)
target_compile_options(acfa_spu PRIVATE -Wall -Wextra)

add_executable(acfa-spurecomp src/spurecomp/main.cpp)
target_link_libraries(acfa-spurecomp PRIVATE acfa_spu)
target_compile_options(acfa-spurecomp PRIVATE -Wall -Wextra)

# ---- recompiler ----
add_executable(acfa-recomp
  src/recomp/main.cpp
  src/recomp/image.cpp
  src/recomp/analysis.cpp
  src/recomp/emit.cpp)
target_link_libraries(acfa-recomp PRIVATE acfa_ppu)
target_compile_options(acfa-recomp PRIVATE -Wall -Wextra)

# ---- generated PPU code (game-derived, from local/gen/ppu; never distributed) ----
set(ACFA_GEN_DIR "${CMAKE_SOURCE_DIR}/local/gen/ppu" CACHE PATH "Directory with acfa-recomp output")
file(GLOB ACFA_GEN_SRC CONFIGURE_DEPENDS "${ACFA_GEN_DIR}/*.cpp")
set(ACFA_CPU_FLAGS -march=haswell)  # AVX2 + FMA + BMI2 (x86-64-v3)

if(ACFA_GEN_SRC)
  add_library(acfa_ppu_gen STATIC ${ACFA_GEN_SRC})
  target_include_directories(acfa_ppu_gen PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(acfa_ppu_gen PRIVATE -O2 ${ACFA_CPU_FLAGS} -fno-strict-aliasing -w)

  file(GLOB ACFA_RT_SRC CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/runtime/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/hle/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/lv2/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/rsx/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/gpu/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/audio/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/media/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/game/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/spu/*.cpp)
  # Recompiled SPU code (game-derived, from local/gen/spu; never distributed).
  set(ACFA_SPU_GEN_DIR "${CMAKE_SOURCE_DIR}/local/gen/spu" CACHE PATH "Directory with acfa-spurecomp output")
  file(GLOB ACFA_SPU_GEN_SRC CONFIGURE_DEPENDS "${ACFA_SPU_GEN_DIR}/*.cpp")
  if(NOT ACFA_SPU_GEN_SRC)
    set(ACFA_SPU_GEN_SRC ${CMAKE_SOURCE_DIR}/src/spu/spu_gen_empty.cpp)
  endif()
  add_library(acfa_spu_gen STATIC ${ACFA_SPU_GEN_SRC})
  target_include_directories(acfa_spu_gen PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(acfa_spu_gen PRIVATE -O2 ${ACFA_CPU_FLAGS} -fno-strict-aliasing -w)

  add_executable(acfa ${ACFA_RT_SRC})
  target_include_directories(acfa PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(acfa PRIVATE ${ACFA_CPU_FLAGS} -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter -g)
  # Whole-archive: the function table references everything anyway; keeps HLE registrars alive.
  target_link_libraries(acfa PRIVATE acfa_ppu_gen acfa_spu_gen acfa_spu acfa_ppu acfa_common d3d11 dxgi d3dcompiler ole32 psapi
                        synchronization)
endif()

# ---- preparation tool ----
add_executable(acfa-prep
  src/prep/main.cpp
  src/prep/pkg.cpp
  src/prep/self.cpp
  src/prep/firmware.cpp
  src/prep/prx.cpp
  src/prep/sfo.cpp)
target_link_libraries(acfa-prep PRIVATE acfa_common)
target_compile_options(acfa-prep PRIVATE -Wall -Wextra)

# ---- media decoder test tool (development: checks the MPEG-2 decoder on the user's movies) ----
add_executable(acfa-vdectest src/tools/vdectest.cpp src/runtime/media/mpeg2_video.cpp src/runtime/gpu/png.cpp)
target_include_directories(acfa-vdectest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(acfa-vdectest PRIVATE acfa_common mfplat mfuuid ole32 winmm)
target_compile_options(acfa-vdectest PRIVATE ${ACFA_CPU_FLAGS} -Wall -Wextra -Wno-unused-parameter -g)
