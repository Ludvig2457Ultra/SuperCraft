/**
 * SuperCraft · Live Commit Race
 *
 * Никаких таймеров_progress и «турбо». Все проценты приходят из /api/progress,
 * который считает реальные коммиты в локальных git-репозиториях.
 */

const POLL_MS = 2000;

const ids = [
  'clock', 'bugPct', 'bugCount', 'usTotal', 'usFill', 'usRunner', 'usPct',
  'usStage', 'usRecent', 'usTotalFact', 'usLast', 'fixList', 'themTotal',
  'themFill', 'themRunner', 'themPct', 'themStage', 'themRecent',
  'themTotalFact', 'themLast', 'bugFill', 'bugLabel', 'tSc', 'tMcl',
  'gap', 'uptime', 'apiState', 'refresh',
];
const dom = Object.fromEntries(ids.map((id) => [id, document.getElementById(id)]));

let timer = 0;
let lastSignature = '';

function clamp(value) {
  return Math.max(0, Math.min(100, Number(value) || 0));
}

function setTrack(fill, runner, percent) {
  const pct = clamp(percent);
  fill.style.transform = `scaleX(${(pct / 100).toFixed(4)})`;
  runner.style.left = `${Math.max(1, Math.min(99, pct)).toFixed(2)}%`;
}

function fmtDuration(seconds) {
  const s = Math.max(0, Math.floor(seconds || 0));
  return `${String(Math.floor(s / 60)).padStart(2, '0')}:${String(s % 60).padStart(2, '0')}`;
}

function clockText(unix) {
  const d = new Date((unix || Date.now() / 1000) * 1000);
  return d.toLocaleTimeString('ru-RU', { hour12: false });
}

function renderCommits(node, commits) {
  if (!commits || !commits.length) return '—';
  const c = commits[0];
  return `<b>${c.hash}</b> · ${c.date} · ${c.subject}`;
}

function renderFixes(bugs) {
  const signature = bugs.map((b) => `${b.id}:${b.fixed}`).join('|');
  if (signature === lastSignature) return;
  lastSignature = signature;

  dom.fixList.replaceChildren();
  for (const bug of bugs) {
    const li = document.createElement('li');
    li.className = `fix ${bug.fixed ? 'fix--done' : 'fix--todo'}`;
    li.innerHTML = `
      <span class="fix__dot"></span>
      <span class="fix__name"></span>
      <span class="fix__tag"></span>
      <span class="fix__progress"><span></span></span>
    `;
    li.querySelector('.fix__name').textContent = bug.name;
    li.querySelector('.fix__tag').textContent = bug.fixed ? '✓ закоммичено' : 'ждёт коммита';
    li.querySelector('.fix__progress span').style.width = bug.fixed ? '100%' : '0%';
    dom.fixList.appendChild(li);
  }
}

function render(data) {
  const sc = data.supercraft;
  const mcl = data.mcpelauncher;
  const bugs = data.bugfix;

  setTrack(dom.usFill, dom.usRunner, sc.percent);
  setTrack(dom.themFill, dom.themRunner, mcl.percent);

  dom.usPct.textContent = `${sc.percent.toFixed(1)}%`;
  dom.themPct.textContent = `${mcl.percent.toFixed(1)}%`;
  dom.usStage.textContent = 'коммиты за 30 дней';
  dom.themStage.textContent = 'коммиты за 30 дней';

  dom.usTotal.textContent = `${sc.total} коммитов`;
  dom.themTotal.textContent = `${mcl.total} коммитов`;
  dom.usTotalFact.textContent = String(sc.total);
  dom.themTotalFact.textContent = String(mcl.total);
  dom.usRecent.textContent = String(sc.recent30);
  dom.themRecent.textContent = String(mcl.recent30);

  dom.usLast.innerHTML = renderCommits(null, sc.commits);
  dom.themLast.innerHTML = renderCommits(null, mcl.commits);

  dom.bugPct.textContent = `${bugs.percent.toFixed(1)}%`;
  dom.bugCount.textContent = `${bugs.fixed} / ${bugs.total}`;
  dom.bugFill.style.width = `${bugs.percent.toFixed(1)}%`;
  dom.bugLabel.textContent = bugs.fixed === bugs.total
    ? 'все запланированные баги исправлены'
    : `список: ${bugs.ids.join(', ') || 'пока пусто'}`;

  const gap = sc.recent30 - mcl.recent30;
  dom.gap.textContent = gap > 0 ? `+${gap}` : gap < 0 ? String(gap) : '±0';
  dom.gap.style.color = gap >= 0 ? 'var(--lime)' : 'var(--violet)';

  dom.tSc.textContent = String(sc.total);
  dom.tMcl.textContent = String(mcl.total);
  dom.clock.textContent = clockText(data.now);
  dom.uptime.textContent = fmtDuration(data.uptime);
  dom.apiState.textContent = 'git · ok';

  renderFixes(data.bugs);
}

async function refresh() {
  try {
    const response = await fetch(`/api/progress?t=${Date.now()}`, { cache: 'no-store' });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    render(await response.json());
  } catch (error) {
    dom.apiState.textContent = 'нет связи';
    dom.apiState.style.color = 'var(--red)';
  }
}

dom.refresh.addEventListener('click', refresh);
refresh();
timer = setInterval(refresh, POLL_MS);
