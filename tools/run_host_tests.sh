#!/usr/bin/env bash
# Build and run the host test project (test_host/) on the ESP-IDF linux target; the exit status is the test run's.
# idf.py set-target runs only when there is no linux build yet or an sdkconfig.defaults* file is newer than sdkconfig.
# Requires an activated ESP-IDF environment.
set -euo pipefail

dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../test_host" && pwd)"

needs_set_target() {
    local defaults
    grep -qx 'IDF_TARGET:STRING=linux' "$dir/build/CMakeCache.txt" 2>/dev/null || return 0
    [[ -f "$dir/sdkconfig" ]] || return 0
    for defaults in "$dir"/sdkconfig.defaults*; do
        [[ -f "$defaults" && "$defaults" -nt "$dir/sdkconfig" ]] && return 0
    done
    return 1
}

if needs_set_target; then
    idf.py -C "$dir" --preview set-target linux
fi
idf.py -C "$dir" build
# The ELF of the current project() name: a renamed project leaves its old ELF in build/.
elf="$(sed -n 's/.*"app_elf": *"\([^"]*\)".*/\1/p' "$dir/build/project_description.json")"
if [[ -z "$elf" || ! -f "$dir/build/$elf" ]]; then
    echo "no .elf produced in $dir/build" >&2
    exit 1
fi
"$dir/build/$elf"
