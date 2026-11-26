#!/usr/bin/env bash
# Трансляция экрана на YouTube через RTMP.
#
# Ключ НИГДЕ не печатается и не попадает в git. Два способа передать его:
#   export YT_STREAM_KEY='...'            # в текущем терминале
#   printf '%s' '...' > ~/.yt_stream_key  # файл с правами 600
#
# Подстройки: DISPLAY, RES, FPS, VIDEO_BITRATE, AUDIO=1 (микрофон/системный звук).
set -euo pipefail

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

# В некоторых окружениях DNS не резолвит youtube.com, но сеть до Google есть.
# Поднимаем адрес через DNS-over-HTTPS и подставляем IP в RTMP-URL.
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

INPUTS=(-f x11grab -draw_mouse 0 -r "$FPS" -video_size "$RES" -i "$GRAB_INPUT")
if [[ "${AUDIO:-0}" == "1" ]]; then
  INPUTS+=(-f pulse -i "${AUDIO_SOURCE:-default}")
else
  INPUTS+=(-f lavfi -i anullsrc=r=44100:cl=stereo)
fi

if [[ "${1:-}" == "--dry-run" ]]; then
  printf 'ffmpeg %s … -f flv rtmp://%s/live2/<КЛЮЧ СКРЫТ>\n' "${INPUTS[*]}" "$RTMP_URL_HOST"
  exit 0
fi

# -tune zerolatency и короткий GOP — низкая задержка для «живого» стрима.
exec ffmpeg -hide_banner -y "${INPUTS[@]}" \
  -vf "scale=${RES}:flags=bicubic,format=yuv420p" \
  -c:v libx264 -preset veryfast -tune zerolatency \
  -g "$((FPS * 2))" -keyint_min "$FPS" -sc_threshold 0 \
  -b:v "$BITRATE" -maxrate "$BITRATE" -bufsize "$((2 * ${BITRATE%k}))k" \
  -c:a aac -b:a 128k -ar 44100 \
  -f flv "$RTMP_URL"
