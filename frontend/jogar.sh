#!/usr/bin/env bash
set -eu
root="$(dirname -- "$(realpath -- "${BASH_SOURCE[0]}")")"
mkdir -p "$root/saves" "$root/logs"
exec "$root/zeroteam" --assets "$root/assets" --saves "$root/saves" "$@" >>"$root/logs/game.log" 2>&1
