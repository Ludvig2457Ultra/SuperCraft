/**
 * SuperCraft — Live Readiness Tracker
 * Every number comes from /api/progress. No timers driving progress, no turbo.
 */

const POLL_MS = 2000;

const TRACKS = [
  { title: "We're Online", artist: 'Miami Nights 1984' },
  { title: 'Crystal Skies (Sped Up)', artist: 'VXLLAIN · iGRES · ENXK' },
  { title: 'Night Drive', artist: 'X-Wave Testarossa' },
  { title: 'Falling', artist: 'Syntarion' },
  { title: 'Night Pulse 105', artist: 'Wizhum' },
];

const ids = [
  'clock', 'heroRemain', 'heroBar', 'heroFoot', 'mclCard', 'mclStatus', 'mclBar',
  'mclHint', 'mclPct', 'scCard', 'scStatus', 'scBar', 'scHint', 'scPct',
  'fixPct', 'fixCount', 'fixBar', 'fixList', 'trackTitle', 'trackArtist',
  'trackDots', 'apiState',
];
const dom = Object.fromEntries(ids.map((id) => [id, document.getElementById(id)]));

let fixSignature = '';
let trackIndex = 0;

const clamp = (n) => Math.max(0, Math.min(100, Number(n) || 0));
const r1 = (n) => Number(n || 0).toFixed(1);
const r0 = (n) => Math.round(Number(n) || 0);

function setBar(node, done) {
  node.style.width = `${clamp(done).toFixed(2)}%`;
}

function clockText(unix) {
  return new Date((unix || Date.now() / 1000) * 1000)
    .toLocaleTimeString('en-GB', { hour12: false });
}

function renderFixes(bugs) {
  const signature = bugs.map((b) => `${b.id}${b.fixed}`).join('|');
  if (signature === fixSignature) return;
  fixSignature = signature;
  dom.fixList.replaceChildren();

  for (const bug of bugs) {
    const li = document.createElement('li');
    li.className = `fix ${bug.fixed ? 'fix--done' : 'fix--open'}`;
    li.innerHTML = '<span class="fix__dot"></span><span class="fix__name"></span><span class="fix__tag"></span>';
    li.querySelector('.fix__name').textContent = bug.name;
    li.querySelector('.fix__tag').textContent = bug.fixed ? 'DONE' : 'OPEN';
    dom.fixList.appendChild(li);
  }
}

function render(data) {
  const sc = data.progress.supercraft;
  const mcl = data.progress.mcpelauncher;
  const bugs = data.bugfix;

  // Hero — SuperCraft is the main subject of the stream.
  dom.heroRemain.innerHTML = `${r0(sc.remaining)}<span>%</span>`;
  dom.heroBar.style.width = `${clamp(sc.done).toFixed(2)}%`;
  dom.heroFoot.textContent = `${r0(sc.done)}% complete · ${sc.state.toLowerCase()}`;

  // Runner cards.
  setBar(dom.scBar, sc.done);
  setBar(dom.mclBar, mcl.done);
  dom.scPct.textContent = `${r0(sc.done)}%`;
  dom.mclPct.textContent = `${r0(mcl.done)}%`;
  dom.scHint.textContent = `${r0(sc.remaining)}% left · ${sc.state.toLowerCase()}`;
  dom.mclHint.textContent = `${r0(mcl.remaining)}% left · ${mcl.state.toLowerCase()}`;
  dom.scStatus.textContent = sc.remaining > 0 ? 'IN DEVELOPMENT' : 'COMPLETE';
  dom.mclStatus.textContent = mcl.remaining > 0 ? 'IN DEVELOPMENT' : 'COMPLETE';
  dom.scCard.classList.toggle('runner--final', sc.remaining === 0);
  dom.mclCard.classList.toggle('runner--final', mcl.remaining === 0);

  // Fixes.
  dom.fixPct.textContent = `${r1(bugs.percent)}%`;
  dom.fixCount.textContent = `${bugs.fixed} / ${bugs.total}`;
  setBar(dom.fixBar, bugs.percent);
  renderFixes(data.bugs);

  dom.clock.textContent = clockText(data.now);
  dom.apiState.textContent = '● CONNECTED';
  dom.apiState.style.color = '';
}

function renderTrack() {
  const track = TRACKS[trackIndex % TRACKS.length];
  dom.trackTitle.textContent = track.title;
  dom.trackArtist.textContent = track.artist;
  dom.trackDots.replaceChildren();
  TRACKS.forEach((_, i) => {
    const dot = document.createElement('i');
    dot.className = i === trackIndex % TRACKS.length ? 'is-active' : '';
    dom.trackDots.appendChild(dot);
  });
  trackIndex += 1;
}

async function refresh() {
  try {
    const response = await fetch(`/api/progress?t=${Date.now()}`, { cache: 'no-store' });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    render(await response.json());
  } catch {
    dom.apiState.textContent = '● OFFLINE';
    dom.apiState.style.color = 'var(--red)';
  }
}

renderTrack();
setInterval(renderTrack, 7000);
refresh();
setInterval(refresh, POLL_MS);
