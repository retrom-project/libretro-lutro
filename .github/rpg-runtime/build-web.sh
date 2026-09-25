#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
output=${1:?absolute empty output directory is required}
python3 "$root/.github/rpg-runtime/candidate_descriptor.py" prepare "$output"
mkdir -p "$root/.retrom-build"
work=$(mktemp -d "$root/.retrom-build/retrom-lutro-web.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM
mkdir -p "$work/raw" "$work/build"
source_digest=$(python3 "$root/.github/rpg-runtime/candidate_descriptor.py" digest "$output")
python3 "$root/.github/rpg-runtime/candidate_descriptor.py" paths "$output" > "$work/source-files"
tar -C "$root" --null --verbatim-files-from -T "$work/source-files" \
  --mtime='@0' --owner=0 --group=0 --numeric-owner \
  --mode='u+rwX,go+rX,go-w' -cf "$work/source.tar"

export RETROM_HOST_UID="$(id -u)"
export RETROM_HOST_GID="$(id -g)"
if ! docker run --rm --platform linux/amd64 --hostname retrom-lutro \
  --env RETROM_HOST_UID --env RETROM_HOST_GID \
  --volume "$work/source.tar:/source.tar:ro" \
  --volume "$root/.github/rpg-runtime:/recipe:ro" \
  --volume "$work/build:/work" \
  --volume "$work/raw:/output" \
  emscripten/emsdk@sha256:af45409f3199d88db4b1b03af0098532c8fb33a375ac257463eeb0a622870d06 \
  /recipe/build-emulatorjs-core.sh lutro \
  >"$work/build.log" 2>&1; then
  tail -200 "$work/build.log" >&2
  exit 1
fi

if grep -E 'wasm-ld: warning: function signature mismatch:' "$work/build.log" >&2; then
  echo 'Lutro and RetroArch have incompatible function signatures' >&2
  exit 1
fi

test "$source_digest" = "$(python3 "$root/.github/rpg-runtime/candidate_descriptor.py" digest "$output")"
stage="$work/stage"
mkdir -p "$stage"
install -m 0644 "$work/raw/lutro_libretro.js" "$stage/"
install -m 0644 "$work/raw/lutro_libretro.wasm" "$stage/"
cat "$root/LICENSE" "$work/raw/retroarch-COPYING" > "$stage/license.txt"
printf '%s\n' '{"minimumEJSVersion":"4.2.2","version":"1.18"}' > "$stage/build.json"
printf '%s\n' '{"name":"lutro","extensions":["lutro"],"makeoptions":{"buildpath":"./","makescript":"Makefile","arguments":[]},"options":{},"save":false,"license":"LICENSE","repo":"https://github.com/retrom-project/libretro-lutro"}' > "$stage/core.json"
chmod 0644 "$stage/build.json" "$stage/core.json" "$stage/license.txt"

(cd "$stage" && 7z a -mtm=off -mta=off -mtc=off -bd -bso0 -bsp0 -t7z "$output/lutro-wasm.data" \
  lutro_libretro.js lutro_libretro.wasm build.json core.json license.txt)
install -m 0644 "$stage/license.txt" "$output/LICENSE"

gzip -n -c "$work/source.tar" > "$output/source.tar.gz"
