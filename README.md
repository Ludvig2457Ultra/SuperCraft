# SuperCraft — Live Commit Race

Живой эфир разработки: **SuperCraft против mcpelauncher**. Никаких выдуманных
процентов и турбо — полосы и числа берутся из реальных git-репозиториев.

## Что есть

- `index.html` + `assets/` — дашборд гонки коммитов, список багов и статусы.
- `tools/dashboard_server.py` — API `/api/progress`, читает локальные клоны:
  `SuperCraft` и `mcpelauncher-manifest` (путь задаётся `MCL_REPO`).
- `assets/supercraft-loop.wav` — сгенерированный synthwave-луп для эфира
  (источник — `tools/gen_music.py`, лицензий и внешних файлов нет).
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

Процент **багфиксов** равен числу уникальных коммитов с трейлером вида:

```
Bug-Fix: open-variadic
```

Список ожидаемых идентификаторов задан в `tools/dashboard_server.py` (`BUGS`).
Пока коммита нет — строка остаётся «ждёт коммита», процент не двигается.
Полосы «коммиты за 30 дней» тоже считаются из `git rev-list` обеих репозиториев.

## Раннер

```bash
cd runner
make
```

Диагностические утилиты: `tools/tdump1.py`, `tools/pickmain.py`, `tools/tcpu.py`.
Известный блокер: загрузка останавливается на 43% после снятия XCB-дедлока —
этот баг ещё **`Bug-Fix: 43pct`**, коммита по нему пока нет.
