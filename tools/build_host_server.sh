#!/usr/bin/env bash
# Build the host server (host/server/) on the ESP-IDF linux target. Requires an activated ESP-IDF environment.
# idf.py set-target runs only when there is no linux build yet or sdkconfig.defaults is newer than sdkconfig.
set -euo pipefail

dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../host/server" && pwd)"

if ! grep -qx 'IDF_TARGET:STRING=linux' "$dir/build/CMakeCache.txt" 2>/dev/null ||
    [[ ! -f "$dir/sdkconfig" || "$dir/sdkconfig.defaults" -nt "$dir/sdkconfig" ]]; then
    idf.py -C "$dir" --preview set-target linux
fi
idf.py -C "$dir" build
