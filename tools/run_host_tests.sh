#!/usr/bin/env bash
# Build and run the host test projects on the ESP-IDF linux target: test_host/ (every feature on) and
# test_host/features_off/ (every CONFIG_TELEMETRIX_* feature off). The exit status is non-zero when either run fails.
# idf.py set-target runs only when there is no linux build yet or an sdkconfig.defaults* file is newer than sdkconfig.
# Requires an activated ESP-IDF environment.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../test_host" && pwd)"

needs_set_target() {
    local dir="$1" defaults
    grep -qx 'IDF_TARGET:STRING=linux' "$dir/build/CMakeCache.txt" 2>/dev/null || return 0
    [[ -f "$dir/sdkconfig" ]] || return 0
    for defaults in "$dir"/sdkconfig.defaults*; do
        [[ -f "$defaults" && "$defaults" -nt "$dir/sdkconfig" ]] && return 0
    done
    return 1
}

run_project() {
    local dir="$1" elf
    if needs_set_target "$dir"; then
        idf.py -C "$dir" --preview set-target linux || return 1
    fi
    idf.py -C "$dir" build || return 1   # set -e does not apply inside a function called with ||
    # The ELF of the current project() name: a renamed project leaves its old ELF in build/.
    elf="$(sed -n 's/.*"app_elf": *"\([^"]*\)".*/\1/p' "$dir/build/project_description.json")"
    if [[ -z "$elf" || ! -f "$dir/build/$elf" ]]; then
        echo "no .elf produced in $dir/build" >&2
        return 1
    fi
    "$dir/build/$elf"
}

status=0
run_project "$root" || status=1
run_project "$root/features_off" || status=1
exit $status
