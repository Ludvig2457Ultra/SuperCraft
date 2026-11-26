# SuperCraft — Live Commit Race

Живой эфир разработки: **SuperCraft против mcpelauncher**. Никаких выдуманных
процентов и турбо — полосы и числа берутся из реальных git-репозиториев.

## Что есть

- `index.html` + `assets/` — дашборд гонки коммитов, список багов и статусы.
- `tools/dashboard_server.py` — API `/api/progress`, читает локальные клоны:
  `SuperCraft` и `mcpelauncher-manifest` (путь задаётся `MCL_REPO`).
- `assets/supercraft-hits.mp3` — 14:32 микс из пяти треков для эфира:
  Miami Nights 1984 «We're Online», VXLLAIN/iGRES/ENXK «Crystal Skies (Sped Up)»,
  X-Wave Testarossa «Night Drive», Syntarion «Falling», Wizhum «Night Pulse 105».
- `tools/launch-live.sh` — поднимает дашборд, Firefox (X11/kiosk) и стрим.
- `tools/stream-youtube.sh` — захват **окна Firefox** (не корня экрана) +
  музыка + RTMP с DNS-over-HTTPS fallback.
- `runner/` — исходники нативного раннера Minecraft Bedrock под Linux.

## Запуск эфира

```bash
printf '%s' 'ВАШ_КЛЮЧ_ПОТОКА' > ~/.yt_stream_key
chmod 600 ~/.yt_stream_key
./tools/launch-live.sh
```

Скрипт сам убьёт старые процессы, поднимет `http://localhost:8080`, откроет
дашборд в Firefox и начнёт трансляцию. Проверка без эфира:

```bash
YT_STREAM_KEY=dummy ./tools/stream-youtube.sh --dry-run
```

Где взять ключ потока: YouTube Studio → **Создать → Трансляция → Поток →
Ключ потока**. Инструкция: https://support.google.com/youtube/answer/2907883?hl=ru

## Как растут проценты

Дашборд показывает **сколько осталось**, а не число коммитов. Готовность
SuperCraft = база 2% + по 1% за каждый реальный `Bug-Fix:`; сейчас 10% готово,
90% осталось. mcpelauncher — 100% (завершён).

Процент **багфиксов** равен числу уникальных коммитов с трейлером вида:

```
Bug-Fix: open-variadic
```

Список ожидаемых идентификаторов задан в `tools/dashboard_server.py` (`BUGS`).
Пока коммита нет — строка остаётся «ждёт коммита», процент не двигается.
Полосы «коммиты за 30 дней» тоже считаются из `git rev-list` обеих репозиториев.

## Музыка в эфире

По просьбе автора эфира в трансляции идут конкретные треки:

1. **We're Online — Miami Nights 1984** · https://www.youtube.com/watch?v=JzzLxIinJO0
2. **Crystal Skies (Sped Up) — VXLLAIN, iGRES, ENXK** · https://www.youtube.com/watch?v=sKJvJCsey84
3. **Night Drive — X-Wave Testarossa** · https://www.youtube.com/watch?v=WwzVDEzrXXI
4. **Falling — Syntarion** · https://www.youtube.com/watch?v=alJA1cwzRZw
5. **Night Pulse 105 — Wizhum** · https://www.youtube.com/watch?v=g7ia5QAJ71I

Все пять собраны в `assets/supercraft-hits.mp3` (14:32, 192 kbps) и зациклены в стриме.

⚠️ Это **коммерческая музыка**: YouTube может наложить Content ID, заглушить
запись или выдать страйк каналу. Если эфир нужно делать безопасным — положи
роялти-фри файл и запусти с `EPIC_TRACK=/путь/к/файлу ./tools/launch-live.sh`.

## Раннер

```bash
cd runner
make
```

Диагностические утилиты: `tools/tdump1.py`, `tools/pickmain.py`, `tools/tcpu.py`.
Известный блокер: загрузка останавливается на 43% после снятия XCB-дедлока —
этот баг ещё **`Bug-Fix: 43pct`**, коммита по нему пока нет.
