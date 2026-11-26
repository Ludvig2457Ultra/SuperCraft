#!/usr/bin/env bash
# Локальный предпросмотр экрана загрузки: http://localhost:8080
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-8080}"
printf 'SuperCraft UI → http://localhost:%s\n' "$PORT"
exec python3 -m http.server "$PORT" --directory "$ROOT"
