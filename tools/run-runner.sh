#!/usr/bin/env bash
# Reproducible local launch of the native Bedrock runner.
# Prepares lib/x86_64 + assets from a local Minecraft APK, builds, then runs.
# Nothing here changes git state or touches the live stream.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APK="${APK:-/home/artem/minecraft-1-21-70-03-x86-64-xbox.apk}"
VERSION="${VERSION:-1.21.70.03}"
VDIR="${VDIR:-$HOME/user_data/bedrock/versions/$VERSION}"
GAME="$VDIR/lib/x86_64/libminecraftpe.so"

[[ -r "$APK" ]] || { printf 'APK not found: %s\n' "$APK" >&2; exit 1; }

mkdir -p "$VDIR"
if [[ ! -r "$VDIR/assets/assets/bootstrap.json" ]]; then
  printf 'Unpacking APK assets → %s/assets\n' "$VDIR"
  unzip -o -q "$APK" 'assets/*' -d "$VDIR"
fi
if [[ ! -r "$GAME" ]]; then
  printf 'Unpacking APK libs → %s/lib/x86_64\n' "$VDIR"
  unzip -o -q "$APK" 'lib/x86_64/*' -d "$VDIR"
fi

ln -sfn "$HOME/user_data" "$ROOT/user_data"
make -C "$ROOT/runner" all

LIBDIR="$VDIR/lib/x86_64"
EXTRA=()
for lib in libc++_shared.so libfmod.so libMediaDecoders_Android.so; do
  [[ -r "$LIBDIR/$lib" ]] && EXTRA+=(--lib "$LIBDIR/$lib")
done

cd "$ROOT/runner"
exec env LD_LIBRARY_PATH=. SUP_ASSETS_DIR="$VDIR/assets" \
  ./runner "${EXTRA[@]}" "$GAME" "$@"
