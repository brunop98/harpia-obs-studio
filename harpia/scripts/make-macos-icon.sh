#!/bin/bash
set -euo pipefail
if [[ $# != 2 ]]; then
  echo "Usage: make-macos-icon.sh input.png output.icns" >&2
  exit 2
fi
for tool in sips iconutil; do
  command -v "$tool" >/dev/null || { echo "Missing macOS icon tool: $tool" >&2; exit 1; }
done
icon_work=$(mktemp -d)
trap 'rm -rf "$icon_work"' EXIT
icon_set="$icon_work/Harpia.iconset"
mkdir -p "$icon_set" "$(dirname "$2")"
for size in 16 32 128 256 512; do
  sips -z "$size" "$size" "$1" --out "$icon_set/icon_${size}x${size}.png" >/dev/null
  retina=$((size * 2))
  sips -z "$retina" "$retina" "$1" --out "$icon_set/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$icon_set" -o "$2"
