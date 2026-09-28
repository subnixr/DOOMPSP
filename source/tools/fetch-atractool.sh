#!/bin/sh
# Download ATRACTool-Reloaded portable (Sony psp_at3tool.exe), used by `make snd0`.
#   source/tools/fetch-atractool.sh
set -eu
URL="https://github.com/XyLe-GBP/ATRACTool-Reloaded/releases/download/v1.52.2620.901/ATRACTool-Rel-Portable.zip"
DIR="$(cd "$(dirname "$0")" && pwd)"
DEST="$DIR/ATRACTool-Rel-Portable"

[ -f "$DEST/res/psp_at3tool.exe" ] && { echo "already present: $DEST"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

curl -fL -o "$TMP/atrac.zip" "$URL"
unzip -q "$TMP/atrac.zip" -d "$TMP"	# zip root: release-portable/
chmod -R u+w "$TMP/release-portable"	# zip stores res/ read-only
rm -rf "$DEST"
mv "$TMP/release-portable" "$DEST"
echo "installed: $DEST"
