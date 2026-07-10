#!/usr/bin/env bash
# Build the shippable STOKER packages into dist/ (GUI ONLY since v2.6.2; the
# TUI still builds for local use but is not distributed):
#   dist/stoker-windows-x64.zip      (static stoker-gui.exe, no DLLs needed)
#   dist/stoker-linux-x86_64.tar.gz  (static linux stoker-gui binary)
# FTXUI v5.0.0 is fetched by CMake on first run. To reuse an already-fetched
# copy: export FETCHCONTENT_SOURCE_DIR_FTXUI=/path/to/ftxui-src
set -euo pipefail
cd "$(dirname "$0")"

EXTRA=()
if [ -n "${FETCHCONTENT_SOURCE_DIR_FTXUI:-}" ]; then
  EXTRA+=("-DFETCHCONTENT_SOURCE_DIR_FTXUI=$FETCHCONTENT_SOURCE_DIR_FTXUI")
fi

echo "== linux =="
cmake -S . -B build-linux -DSTOKER_STATIC=ON "${EXTRA[@]}" >/dev/null
cmake --build build-linux -j"$(nproc)"

echo "== windows (mingw cross) =="
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw64.cmake "${EXTRA[@]}" >/dev/null
cmake --build build-win -j"$(nproc)"

mkdir -p dist
rm -f dist/stoker-linux-x86_64.tar.gz dist/stoker-windows-x64.zip  # zip APPENDS to existing archives
tar -C build-linux -czf dist/stoker-linux-x86_64.tar.gz stoker-gui
( cd build-win && zip -q9 ../dist/stoker-windows-x64.zip stoker-gui.exe )
ls -lh dist/
