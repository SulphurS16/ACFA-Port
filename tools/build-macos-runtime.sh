#!/bin/sh
# Compile/link only. Does not launch a game or change emulator/user settings.
set -eu
acfa_source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
acfa_build_dir="${1:-$acfa_source_dir/../local/build-codegen}"
cmake -S "$acfa_source_dir" -B "$acfa_build_dir" \
  -DACFA_GAME_PROFILE=jp-bljm55005 -DACFA_CONTROLLER_ONLY=ON \
  -DACFA_BUILD_RUNTIME=ON -DACFA_EXPERIMENTAL_MAC_RUNTIME=ON \
  -DACFA_REQUIRE_METAL_BRIDGE=ON \
  -DACFA_GEN_DIR="$acfa_source_dir/../local/gen/ppu" \
  -DACFA_SPU_GEN_DIR="$acfa_source_dir/../local/gen/spu"
cmake --build "$acfa_build_dir" --target acfa --parallel 4
