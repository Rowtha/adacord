#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source_file="$root/installer/linux/adacord_installer.py"
output="${1:-"$root/dist/AdacordInstaller-linux"}"
archive="${2:-"$root/dist/AdacordInstaller-linux.tar.gz"}"

command -v python3 >/dev/null
PYTHONPYCACHEPREFIX="${TMPDIR:-/tmp}/adacord-installer-pycache" \
    python3 -m py_compile "$source_file"
python3 -c "import gi; gi.require_version('Gtk', '3.0'); from gi.repository import Gtk"

install -Dm755 "$source_file" "$output"
tar -czf "$archive" -C "$(dirname "$output")" "$(basename "$output")"

echo "Built $output"
echo "Built $archive"
