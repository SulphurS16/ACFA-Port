#!/bin/sh
set -eu
acfa_source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
acfa_build_dir="${1:-$acfa_source_dir/../local/build-host}"
cmake -S "$acfa_source_dir" -B "$acfa_build_dir" \
  -DACFA_REQUIRE_METAL_BRIDGE=ON -DACFA_GAME_PROFILE=jp-bljm55005 -DACFA_CONTROLLER_ONLY=ON -DACFA_BUILD_RUNTIME=OFF
cmake --build "$acfa_build_dir" --parallel 4
ctest --test-dir "$acfa_build_dir" --output-on-failure
