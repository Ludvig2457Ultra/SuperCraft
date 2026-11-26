/**
 * SuperCraft · Live Readiness Tracker
 *
 * Показываем не число коммитов, а «сколько осталось». Процент готовности
 * SuperCraft = база + реальные коммиты с Bug-Fix:. mcpelauncher — завершён.
 */

const POLL_MS = 2000;

const ids = [
  'clock', 'heroRemain', 'heroDone', 'mclChip', 'mclLeft', 'mclDone',
  'mclFill', 'mclRemainder', 'mclLast', 'scChip', 'scLeft', 'scDone',
  'scFill', 'scRemainder', 'scLast', 'bugPct', 'bugCount', 'bugFill',
  'bugLabel', 'fixList', 'tScDone', 'tScLeft', 'tMclDone', 'uptime',
  'apiState', 'refresh',
];
const dom = Object.fromEntries(ids.map((id) => [id, document.getElementById(id)]));

let lastFixSignature = '';

function clamp(value) {
  return Math.max(0, Math.min(100, Number(value) || 0));
}

function setBar(fill, remainder, done) {
  const pct = clamp(done);
  fill.style.transform = `scaleX(${(pct / 100).toFixed(4)})`;
  // Остаток начинается там, где кончается заливка.
  remainder.style.left = `${pct.toFixed(2)}%`;
  remainder.style.right = '0';
}

function fmtDuration(seconds) {
  const s = Math.max(0, Math.floor(seconds || 0));
  return `${String(Math.floor(s / 60)).padStart(2, '0')}:${String(s % 60).padStart(2, '0')}`;
}

function clockText(unix) {
  return new Date((unix || Date.now() / 1000) * 1000)
    .toLocaleTimeString('ru-RU', { hour12: false });
}

function lastCommitLine(repo) {
  const c = repo.commits && repo.commits[0];
  return c ? `<b>${c.hash}</b> · ${c.date} · ${c.subject}` : '—';
}

function renderFixes(bugs) {
  const signature = bugs.map((b) => `${b.id}:${b.fixed}`).join('|');
  if (signature === lastFixSignature) return;
  lastFixSignature = signature;

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
    li.querySelector('.fix__tag').textContent = bug.fixed ? '✓' : '…';
    li.querySelector('.fix__progress span').style.width = bug.fixed ? '100%' : '0%';
    dom.fixList.appendChild(li);
  }
}

function render(data) {
  const sc = data.progress.supercraft;
  const mcl = data.progress.mcpelauncher;
  const bugs = data.bugfix;

  // SuperCraft — главный герой эфира.
  dom.heroRemain.textContent = `${sc.remaining.toFixed(0)}%`;
  dom.heroDone.textContent = `готово ${sc.done.toFixed(0)}%`;
  dom.scDone.textContent = `${sc.done.toFixed(0)}%`;
  dom.scLeft.textContent = `${sc.remaining.toFixed(0)}% осталось`;
  dom.scChip.textContent = sc.remaining > 0 ? 'В РАЗРАБОТКЕ' : 'ЗАВЕРШЁН';
  setBar(dom.scFill, dom.scRemainder, sc.done);
  dom.scLast.innerHTML = lastCommitLine(data.supercraft);

  // mcpelauncher — эталон, уже finished.
  dom.mclDone.textContent = `${mcl.done.toFixed(0)}%`;
  dom.mclLeft.textContent = `${mcl.remaining.toFixed(0)}% осталось`;
  dom.mclChip.textContent = mcl.remaining > 0 ? 'В РАЗРАБОТКЕ' : 'ЗАВЕРШЁН';
  setBar(dom.mclFill, dom.mclRemainder, mcl.done);
  dom.mclLast.innerHTML = lastCommitLine(data.mcpelauncher);

  dom.bugPct.textContent = `${bugs.percent.toFixed(1)}%`;
  dom.bugCount.textContent = `${bugs.fixed} / ${bugs.total}`;
  dom.bugFill.style.width = `${bugs.percent.toFixed(1)}%`;
  dom.bugLabel.textContent = bugs.fixed === bugs.total
    ? 'все запланированные баги исправлены'
    : `ждёт коммита: ${bugs.ids.length ? bugs.ids.join(', ') : 'список пуст'}`;

  dom.tScDone.textContent = `${sc.done.toFixed(0)}%`;
  dom.tScLeft.textContent = `${sc.remaining.toFixed(0)}%`;
  dom.tMclDone.textContent = `${mcl.done.toFixed(0)}%`;
  dom.clock.textContent = clockText(data.now);
  dom.uptime.textContent = fmtDuration(data.uptime);
  dom.apiState.textContent = 'git · ok';
  dom.apiState.style.color = '';

  renderFixes(data.bugs);
}

async function refresh() {
  try {
    const response = await fetch(`/api/progress?t=${Date.now()}`, { cache: 'no-store' });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    render(await response.json());
  } catch {
    dom.apiState.textContent = 'нет связи';
    dom.apiState.style.color = 'var(--red)';
  }
}

dom.refresh.addEventListener('click', refresh);
refresh();
setInterval(refresh, POLL_MS);
