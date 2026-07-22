#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
compiler="${CC:-x86_64-w64-mingw32-gcc}"
windres="${WINDRES:-x86_64-w64-mingw32-windres}"
output="${1:-"$root/dist/AdacordInstaller.exe"}"
build_dir="${TMPDIR:-/tmp}/adacord-installer-build"

command -v "$compiler" >/dev/null
command -v "$windres" >/dev/null
command -v node >/dev/null

mkdir -p "$build_dir" "$(dirname "$output")"
node "$root/installer/windows/make-icon.mjs" \
    "$root/browser/icon.png" \
    "$root/installer/windows/adacord.ico"

(
    cd "$root/installer/windows"
    "$windres" \
        --input adacord_installer.rc \
        --output "$build_dir/adacord_installer.res.o"
)

"$compiler" \
    -std=c11 \
    -O2 \
    -Wall \
    -Wextra \
    -Werror \
    -municode \
    -mwindows \
    -static \
    -o "$output" \
    "$root/installer/windows/adacord_installer.c" \
    "$build_dir/adacord_installer.res.o" \
    -lcomctl32 \
    -lshell32 \
    -lole32 \
    -lgdi32 \
    -ladvapi32

echo "Built $output"
