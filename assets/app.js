/**
 * SuperCraft — демонстрационный экран загрузки.
 *
 * state — единственный источник правды; DOM обновляется только когда значение
 * реально изменилось. Анимация идёт через requestAnimationFrame: вкладка не
 * уходит в простой, а ползунок не дёргается на границах кадров.
 */

const STAGES = [
  { at: 0,   title: 'Подготовка чанков',     detail: 'Инициализация рендера' },
  { at: 18,  title: 'Загрузка мира',         detail: 'Декодирование чанков' },
  { at: 42,  title: 'Компиляция шейдеров',   detail: 'llvmpipe · GLSL кэш' },
  { at: 66,  title: 'Синхронизация активов', detail: 'Пакеты и текстуры' },
  { at: 88,  title: 'Почти готово',          detail: 'Запуск игрового цикла' },
  { at: 100, title: 'Мир готов',             detail: 'Добро пожаловать в SuperCraft' },
];

const BASE_RATE = 26;    // процентов в секунду на обычной скорости
const TURBO_FACTOR = 2;  // множитель кнопки «Турбо»
const TELEMETRY_MS = 80; // как часто пересчитывать циферблаты

const dom = {
  root: document.documentElement,
  stage: document.getElementById('stage-title'),
  percent: document.getElementById('percent'),
  detail: document.getElementById('detail'),
  eta: document.getElementById('eta'),
  progress: document.getElementById('progress'),
  fill: document.getElementById('fill'),
  fps: document.getElementById('fps'),
  chunks: document.getElementById('chunks'),
  mem: document.getElementById('mem'),
  throughput: document.getElementById('throughput'),
  restart: document.getElementById('restart'),
  turbo: document.getElementById('turbo'),
};

const state = {
  value: 0,
  shown: -1,
  turbo: false,
  running: true,
  lastFrame: 0,
  lastTelemetry: 0,
  stageIndex: -1,
};

/** Плавная скорость: разгон в начале, мягкий спад на финише. */
function rateAt(value) {
  const curve = 0.65 + 0.9 * Math.sin((Math.PI * value) / 100);
  return BASE_RATE * curve * (state.turbo ? TURBO_FACTOR : 1);
}

function stageAt(value) {
  let index = 0;
  for (let i = 1; i < STAGES.length; i += 1) {
    if (value >= STAGES[i].at) index = i;
  }
  return index;
}

function renderFrame() {
  const value = state.value;
  const rounded = Math.min(100, Math.floor(value));

  // transform дешевле width: не пересчитывается layout родителя.
  dom.fill.style.transform = `scaleX(${(value / 100).toFixed(4)})`;

  if (rounded !== state.shown) {
    state.shown = rounded;
    dom.percent.textContent = String(rounded);
    dom.progress.setAttribute('aria-valuenow', String(rounded));
    const remaining = Math.max(0, (100 - value) / rateAt(value));
    dom.eta.textContent = remaining < 0.05 ? 'готово' : `≈ ${remaining.toFixed(1)} с`;
  }

  const index = stageAt(value);
  if (index !== state.stageIndex) {
    state.stageIndex = index;
    dom.stage.textContent = STAGES[index].title;
    dom.detail.textContent = STAGES[index].detail;
  }
}

function renderTelemetry(now) {
  if (now - state.lastTelemetry < TELEMETRY_MS) return;
  state.lastTelemetry = now;

  const rate = rateAt(state.value);
  const wave = Math.sin(now / 170);
  const jitter = Math.sin(now / 61) * 0.018;

  dom.fps.textContent = String(Math.round(59 + wave * 2 + jitter * 100));
  dom.chunks.textContent = String(Math.round(rate * (4.2 + jitter)));
  dom.mem.textContent = `${Math.round(184 + state.value * 3.4)} МБ`;
  dom.throughput.textContent = `${(rate * (2.6 + jitter)).toFixed(1)} Мбит/с`;
}

function frame(now) {
  if (!state.running) return;

  const dt = state.lastFrame ? Math.min((now - state.lastFrame) / 1000, 0.05) : 0;
  state.lastFrame = now;

  state.value = Math.min(100, state.value + rateAt(state.value) * dt);
  dom.root.style.setProperty('--boost', state.turbo ? '2' : '1');

  renderFrame();
  renderTelemetry(now);

  if (state.value >= 100) {
    state.running = false;
    dom.restart.textContent = 'Ещё раз';
    dom.eta.textContent = 'готово';
  } else {
    requestAnimationFrame(frame);
  }
}

function restart() {
  state.value = 0;
  state.shown = -1;
  state.stageIndex = -1;
  state.lastFrame = 0;
  state.running = true;
  dom.restart.textContent = 'Перезапустить';
  requestAnimationFrame(frame);
}

function toggleTurbo() {
  state.turbo = !state.turbo;
  dom.turbo.setAttribute('aria-pressed', String(state.turbo));
  dom.turbo.textContent = state.turbo ? 'Турбо ×2 · вкл' : 'Турбо ×2';
}

dom.restart.addEventListener('click', restart);
dom.turbo.addEventListener('click', toggleTurbo);

requestAnimationFrame(frame);
