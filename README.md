# SuperCraft — Live Commit Race

Живой эфир разработки: **SuperCraft против mcpelauncher**. Никаких выдуманных
процентов и турбо — полосы и числа берутся из реальных git-репозиториев.

## Что есть

- `index.html` + `assets/` — дашборд гонки коммитов, список багов и статусы.
- `tools/dashboard_server.py` — API `/api/progress`, читает локальные клоны:
  `SuperCraft` и `mcpelauncher-manifest` (путь задаётся `MCL_REPO`).
- `assets/supercraft-synthwave.mp3` — 9:52 synthwave/outrun-микс для эфира
  (по стилю близок к Miami Nights 1984 / DOS-88). Три роялти-фри трека,
  склеенные в один файл, чтобы не было пауз при смене. Источники ниже.
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

Треки Miami Nights 1984 и «Crystal Skies» с YouTube —
коммерческие, их нельзя просто скачать и пустить в трансляцию: это Content ID
и страйк каналу. Поэтому в эфире идёт похожий по звуку **роялти-фри** микс:

1. **Outrun Boulevard** — Kjartan Abel, [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/),
   https://kjartan-abel.com/library/outrun-boulevard/
2. **Far Away** — DOS-88, [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/),
   https://dos88.bandcamp.com/
3. **Retrowave** — Raspberrymusic, [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/),
   https://raspberrysounds.bandcamp.com/track/retrowave-no-copyright-music-by-raspberrymusic

Файлы взяты с Wikimedia Commons и склеены в `assets/supercraft-synthwave.mp3`.
Авторство указано выше, как требуют лицензии. Заменить трек можно через
`EPIC_TRACK=/путь/к/файлу ./tools/launch-live.sh`.

## Раннер

```bash
cd runner
make
```

Диагностические утилиты: `tools/tdump1.py`, `tools/pickmain.py`, `tools/tcpu.py`.
Известный блокер: загрузка останавливается на 43% после снятия XCB-дедлока —
этот баг ещё **`Bug-Fix: 43pct`**, коммита по нему пока нет.
