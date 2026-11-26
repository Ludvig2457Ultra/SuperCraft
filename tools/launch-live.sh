#!/usr/bin/env bash
# Поднимает весь живой стенд: дашборд → Firefox (X11, kiosk) → YouTube-стрим.
# Ключ берётся из ~/.yt_stream_key или YT_STREAM_KEY.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_DIR="${LOG_DIR:-/home/artem}"
XA="$(ls -t /run/user/$(id -u)/xauth_* 2>/dev/null | head -1 || true)"

printf 'Останавливаю старый эфир…\n'
pkill -x ffmpeg 2>/dev/null || true
pkill -x firefox 2>/dev/null || true
pkill -f '[d]ashboard_server.py' 2>/dev/null || true
pkill -f '[h]ttp.server 8080' 2>/dev/null || true

printf 'Запускаю дашборд (реальные коммиты из git)…\n'
setsid python3 "$ROOT/tools/dashboard_server.py" \
  > "$LOG_DIR/dashboard.log" 2>&1 < /dev/null &

for _ in 1 2 3 4 5 6 7 8 9 10; do
  curl -fsS http://localhost:8080/api/progress >/dev/null 2>&1 && break
  sleep 0.4
done

printf 'Запускаю Firefox в X11/kiosk…\n'
PROFILE="${FIREFOX_PROFILE:-$HOME/.ff-supercraft-live}"
rm -rf "$PROFILE"
mkdir -p "$PROFILE"
chmod 700 "$PROFILE"
setsid env -u WAYLAND_DISPLAY -u XDG_SESSION_TYPE \
  DISPLAY="${DISPLAY:-:0}" XAUTHORITY="$XA" \
  MOZ_ENABLE_WAYLAND=0 MOZ_DISABLE_CONTENT_SANDBOX=1 \
  GDK_BACKEND=x11 XDG_SESSION_TYPE=x11 \
  /usr/lib/firefox/firefox --no-remote --profile "$PROFILE" --kiosk \
  http://localhost:8080 > "$LOG_DIR/firefox-live.log" 2>&1 < /dev/null &

sleep 9

printf 'Запускаю стрим с музыкой…\n'
setsid env DISPLAY="${DISPLAY:-:0}" XAUTHORITY="$XA" \
  "$ROOT/tools/stream-youtube.sh" \
  > "$LOG_DIR/yt-stream.log" 2>&1 < /dev/null &

sleep 8
FFMPEG_PID="$(pgrep -x ffmpeg | head -1 || true)"
if [[ -z "$FFMPEG_PID" ]]; then
  printf 'ОШИБКА: ffmpeg не запустился. Смотри %s/yt-stream.log\n' "$LOG_DIR" >&2
  tail -12 "$LOG_DIR/yt-stream.log" >&2 || true
  exit 1
fi

printf 'Эфир идёт: ffmpeg=%s · дашборд http://localhost:8080\n' "$FFMPEG_PID"
printf 'Логи: %s/{dashboard,firefox-live,yt-stream}.log\n' "$LOG_DIR"
