#!/usr/bin/env bash
# Build the native Zero Team port from the player's own arcade set.
#
#   bash build.sh "/path/to/Zero-Team-U-S-A_Arcade_EN.zip"
#
# The script verifies the 13 chips, generates the translated V30 program,
# builds both the translated board and the reference interpreter, proves they
# agree frame by frame, then assembles the distribution in .work/dist.
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 path/to/zeroteam.zip (or its outer arcade ZIP)" >&2
    exit 2
fi
rom="$(realpath -- "$1")"
project="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export ZEROTEAM_WORK="$(realpath -m -- "${ZEROTEAM_WORK:-$project/.work}")"
work="$ZEROTEAM_WORK"
[[ "$(uname -sm)" == "Linux x86_64" ]] || { echo "Requires Linux x86-64" >&2; exit 1; }
pkg-config --exists sdl2 zlib || { echo "Missing SDL2 and zlib development files" >&2; exit 1; }

mkdir -p "$work/build/generated" "$work/vendor" "$work/private"
python3 "$project/tools/prepare_chips.py" "$rom" "$work/private/chips" > "$work/private/chip-hashes.json"

vendor="$work/vendor/FBNeo"
revision=61cd3ede225ec44b84011f7679db68123d72725d
if [[ ! -e "$vendor" ]]; then
    git init -q "$vendor"
    git -C "$vendor" remote add origin https://github.com/finalburnneo/FBNeo.git
fi
if ! git -C "$vendor" rev-parse --verify HEAD >/dev/null 2>&1; then
    git -C "$vendor" fetch -q --depth=1 origin "$revision"
    git -C "$vendor" checkout -q --detach FETCH_HEAD
fi
if [[ "$(git -C "$vendor" rev-parse HEAD)" != "$revision" ]]; then
    echo "Build expects FBNeo at $revision in $vendor" >&2
    exit 1
fi

python3 "$project/aot/generate.py" --fbneo "$vendor" --chips "$work/private/chips" \
    --out "$work/build/generated"

python3 "$project/board/build.py" --lib libzeroteam_board_ref.so
python3 "$project/board/build.py" --aot
python3 "$project/tools/verify_board.py" "$work/build" "$work/private/chips" 1200
python3 "$project/tools/verify_edges.py" "$work/build/libzeroteam_board.so" "$work/private/chips"

make -C "$project/frontend" BUILD="$work/build"

dist="$work/dist"
rm -rf "$dist"
mkdir -p "$dist/assets" "$dist/licenses" "$dist/saves" "$dist/logs"
install -m 755 "$work/build/zeroteam" "$work/build/libzeroteam_board.so" "$dist/"
install -m 755 "$project/frontend/jogar.sh" "$dist/"
install -m 644 "$work"/private/chips/*.bin "$dist/assets/"
install -m 644 "$vendor/src/license.txt" "$dist/licenses/FBNeo.txt"
"$dist/zeroteam" --headless --frames 400 --capture "$work/private/selftest.bmp" \
    --assets "$dist/assets" --saves "$dist/saves"
echo "Built your private copy: $dist/jogar.sh"
