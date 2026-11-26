#!/usr/bin/env bash
# Живая трансляция SuperCraft на YouTube.
#
# Ключ потока НЕ печатается и НЕ попадает в git:
#   export YT_STREAM_KEY='...'            # текущий терминал
#   printf '%s' '...' > ~/.yt_stream_key  # файл с правами 600
#
# Захват: окно Firefox (не корень экрана!), музыка — локальный луп из assets/.
# Подстройки: RES, FPS, VIDEO_BITRATE, AUDIO_SOURCE=system|loop|<pulse source>,
#             YT_WINDOW_ID=0x..., DISPLAY.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KEY_FILE="${YT_STREAM_KEY_FILE:-$HOME/.yt_stream_key}"
STREAM_KEY="${YT_STREAM_KEY:-}"

if [[ -z "$STREAM_KEY" && -r "$KEY_FILE" ]]; then
  STREAM_KEY="$(tr -d '[:space:]' < "$KEY_FILE")"
fi

if [[ -z "$STREAM_KEY" ]]; then
  printf 'Нет ключа потока. Укажите YT_STREAM_KEY или создайте %s\n' "$KEY_FILE" >&2
  exit 1
fi

command -v ffmpeg >/dev/null || { printf 'ffmpeg не найден\n' >&2; exit 1; }

DISPLAY_VALUE="${DISPLAY:-:0}"
case "$DISPLAY_VALUE" in
  *.*) GRAB_INPUT="$DISPLAY_VALUE" ;;
  *)   GRAB_INPUT="$DISPLAY_VALUE.0" ;;
esac

RES="${RES:-1280x720}"
FPS="${FPS:-30}"
BITRATE="${VIDEO_BITRATE:-4500k}"
AUDIO_SOURCE="${AUDIO_SOURCE:-loop}"

# ── Выбор окна Firefox ────────────────────────────────────────────────────
# В Wayland-сессии root Xwayland часто чёрный: захватываем само окно браузера.
WIN_ID="${YT_WINDOW_ID:-}"
if [[ -z "$WIN_ID" ]]; then
  WIN_ID="$(DISPLAY="$DISPLAY_VALUE" XAUTHORITY="${XAUTHORITY:-}" python3 - <<'PY' 2>/dev/null || true
from Xlib import display
try:
    d = display.Display()
except Exception:
    raise SystemExit
best = (0, 0)
for w in d.screen().root.query_tree().children:
    try:
        if w.get_attributes().map_state != 2:      # IsViewable
            continue
        g = w.get_geometry()
        area = g.width * g.height
        if g.width >= 800 and g.height >= 600 and area > best[0]:
            best = (area, w.id)
    except Exception:
        pass
print(hex(best[1]) if best[1] else "")
PY
)"
fi

if [[ -n "$WIN_ID" ]]; then
  printf 'Захват окна %s\n' "$WIN_ID" >&2
  GRAB_ARGS=(-f x11grab -draw_mouse 0 -r "$FPS" -video_size "$RES" -window_id "$WIN_ID" -i "$GRAB_INPUT")
else
  printf 'Окно не найдено, захватываю корень экрана (может быть чёрным)\n' >&2
  GRAB_ARGS=(-f x11grab -draw_mouse 0 -r "$FPS" -video_size "$RES" -i "$GRAB_INPUT")
fi

# ── Музыка ────────────────────────────────────────────────────────────────
if [[ "$AUDIO_SOURCE" == "loop" ]]; then
  LOOP="$ROOT/assets/supercraft-loop.wav"
  [[ -r "$LOOP" ]] || { printf 'Нет музыкального лупа: %s\n' "$LOOP" >&2; exit 1; }
  AUDIO_INPUT=(-stream_loop -1 -i "$LOOP")
else
  PULSE_SRC="${AUDIO_SOURCE/system/${PULSE_SOURCE:-default}}"
  AUDIO_INPUT=(-f pulse -i "$PULSE_SRC")
fi

# ── RTMP с DNS-over-HTTPS fallback ────────────────────────────────────────
# В некоторых окружениях DNS не резолвит youtube.com, но сеть до Google есть.
RTMP_HOST="${YT_RTMP_HOST:-a.rtmp.youtube.com}"
RTMP_URL_HOST="$RTMP_HOST"
if ! getent hosts "$RTMP_HOST" >/dev/null 2>&1; then
  DOH_IP="$(curl -fsS --max-time 8 \
    "https://dns.google/resolve?name=${RTMP_HOST}&type=A" | python3 -c '
import json, sys
try:
    ans = json.load(sys.stdin).get("Answer", [])
    print(next((a["data"] for a in ans if a.get("type") == 1), ""))
except Exception:
    print("")
' 2>/dev/null || true)"
  if [[ -n "$DOH_IP" ]]; then
    RTMP_URL_HOST="$DOH_IP"
    printf 'DNS для %s недоступен, использую IP из DoH: %s\n' "$RTMP_HOST" "$DOH_IP" >&2
  fi
fi
RTMP_URL="rtmp://${RTMP_URL_HOST}/live2/${STREAM_KEY}"

if [[ "${1:-}" == "--dry-run" ]]; then
  printf 'ffmpeg %s %s … -f flv rtmp://%s/live2/<КЛЮЧ СКРЫТ>\n' \
    "${GRAB_ARGS[*]}" "${AUDIO_INPUT[*]}" "$RTMP_URL_HOST"
  exit 0
fi

# -tune zerolatency и короткий GOP — низкая задержка для «живого» стрима.
exec ffmpeg -hide_banner -y "${GRAB_ARGS[@]}" "${AUDIO_INPUT[@]}" \
  -vf "scale=${RES}:flags=bicubic,format=yuv420p" \
  -c:v libx264 -preset veryfast -tune zerolatency \
  -g "$((FPS * 2))" -keyint_min "$FPS" -sc_threshold 0 \
  -b:v "$BITRATE" -maxrate "$BITRATE" -bufsize "$((2 * ${BITRATE%k}))k" \
  -c:a aac -b:a 128k -ar 44100 \
  -f flv "$RTMP_URL"
