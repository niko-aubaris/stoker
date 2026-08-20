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
rm -f dist/stoker-linux-x86_64.tar.gz dist/stoker-windows-x64.zip dist/*.sig  # zip APPENDS to existing archives
tar -C build-linux -czf dist/stoker-linux-x86_64.tar.gz stoker-gui
( cd build-win && zip -q9 ../dist/stoker-windows-x64.zip stoker-gui.exe )

# Ed25519-sign both archives; updaters >= v2.10.0 refuse unsigned releases,
# so a release without .sig files bricks self-update for everyone. Hard fail.
echo "== sign =="
if [ ! -f "$HOME/.config/stoker-release/signing.key" ]; then
  echo "FATAL: no signing key at ~/.config/stoker-release/signing.key" >&2
  echo "       (tools/stoker-sign keygen creates one; the pubkey must match" >&2
  echo "       UPDATE_PUBKEY_HEX in bpos-dash.cpp)" >&2
  exit 1
fi
cc -O2 -o build-linux/stoker-sign tools/stoker-sign.c tweetnacl.c
./build-linux/stoker-sign sign dist/stoker-linux-x86_64.tar.gz dist/stoker-windows-x64.zip
./build-linux/stoker-sign verify dist/stoker-linux-x86_64.tar.gz
./build-linux/stoker-sign verify dist/stoker-windows-x64.zip
# sig assets drop the archive extension: pre-2.10 updaters match assets by
# "<platform>.<ext>" substring and would download a *.tar.gz.sig as the package
mv dist/stoker-linux-x86_64.tar.gz.sig dist/stoker-linux-x86_64.sig
mv dist/stoker-windows-x64.zip.sig dist/stoker-windows-x64.sig
echo "upload the .sig files as release assets alongside the archives"
ls -lh dist/
