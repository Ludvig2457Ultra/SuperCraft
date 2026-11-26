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
SuperCraft racing = база 2% + по 0.1% за каждый реальный `Bug-Fix:`; сейчас
3% готово, 97% осталось. mcpelauncher — 100% (завершён).

Процент **багфиксов** равен числу уникальных коммитов с трейлером вида:

```
Bug-Fix: open-variadic
```

Список ожидаемых идентификаторов задан в `tools/dashboard_server.py` (`BUGS`).

Каждый новый коммит появляется прямо в эфире как уведомление **NEW COMMIT**:
название коммита и описание из тела коммита. Уведомление живёт 9 секунд и
не использует размытие, поэтому захват окна Firefox остаётся чистым.
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

## Картинка эфира

- `assets/supercraft-icon.png` — иконка SuperCraft (неоновый куб, нарисована локально).
- `assets/mcpelauncher-icon.png` — иконка mcpelauncher на основе официального
  аватара организации `minecraft-linux` с GitHub (creeper).
- `assets/backgrounds/bedrock-*.jpg` — пять кадров Minecraft: смена каждые 12 секунд
  с плавным приближением/отдалением (Ken Burns). Источники и лицензии —
  `assets/backgrounds/CREDITS.md` (CC BY 3.0, Xbox México / Xbox MENA).
- Подвижные слои фона **не используют `filter: blur()`**: именно они давали
  чёрные кадры при захвате окна Firefox через X11. `launch-live.sh` перед стартом
  стрима отдельно ждёт нечёрного кадра дашборда.

⚠️ Это **коммерческая музыка**: YouTube может наложить Content ID, заглушить
запись или выдать страйк каналу. Если эфир нужно делать безопасным — положи
роялти-фри файл и запусти с `EPIC_TRACK=/путь/к/файлу ./tools/launch-live.sh`.

## Раннер

```bash
./tools/run-runner.sh
```

Скрипт распаковывает из локального APK именно те `assets/` и `lib/x86_64/`,
которые ждёт движок, создаёт симлинк `user_data`, собирает runner и запускает
его с `SUP_ASSETS_DIR`. Без этого наboarding-папка была пустой: `AAssetManager`
возвращал пустые листинги, и загрузка замирала на 43% даже после снятия
XCB-дедлока. Теперь загрузка доходит до EGL и рендера (в логе `SWAPS>`),
а не останавливается на 43%. APK можно задать через `APK=...`, версию — через
`VERSION=...`, каталог данных — через `VDIR=...`.

Диагностические утилиты: `tools/tdump1.py`, `tools/pickmain.py`, `tools/tcpu.py`.
