/**
 * SuperCraft racing — Live Readiness Tracker
 * Every number comes from /api/progress. No timers driving progress, no turbo.
 * Background slideshow is cosmetic only and never blanks the frame: images are
 * preloaded and cross-faded, one layer always stays visible.
 */

const POLL_MS = 2000;
const BG_MS = 12000;

const BACKGROUNDS = [
  'assets/backgrounds/bedrock-1.jpg',
  'assets/backgrounds/bedrock-2.jpg',
  'assets/backgrounds/bedrock-3.jpg',
  'assets/backgrounds/bedrock-4.jpg',
  'assets/backgrounds/bedrock-5.jpg',
];

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
  'trackDots', 'apiState', 'bgPhotos', 'toastStack',
];
const dom = Object.fromEntries(ids.map((id) => [id, document.getElementById(id)]));

let fixSignature = '';
let trackIndex = 0;
let bgIndex = -1;
let lastCommitHash = null;

const clamp = (n) => Math.max(0, Math.min(100, Number(n) || 0));
/* Один знак после запятой обязателен: шаг равен 0.1% за коммит, поэтому
 * округление до целого скрывало каждое новое исправление. */
const r1 = (n) => Number(n || 0).toFixed(1);

function setBar(node, done) {
  node.style.width = `${clamp(done).toFixed(2)}%`;
}

function clockText(unix) {
  return new Date((unix || Date.now() / 1000) * 1000)
    .toLocaleTimeString('en-GB', { hour12: false });
}

function setupBackdrop() {
  const host = dom.bgPhotos;
  if (!host) return;

  const photos = BACKGROUNDS.map((src) => {
    const img = document.createElement('img');
    img.className = 'bg-photo';
    img.src = src;
    img.alt = '';
    img.decoding = 'async';
    host.appendChild(img);
    return img;
  });

  const activate = (next) => {
    if (next === bgIndex) return;
    if (bgIndex >= 0) photos[bgIndex].classList.remove('is-active');
    bgIndex = next;
    photos[bgIndex].classList.add('is-active');
  };

  // Show the first frame early, then rotate only after every image is ready.
  setTimeout(() => { if (bgIndex < 0) activate(0); }, 500);
  Promise.all(photos.map((img) => new Promise((done) => {
    if (img.complete) return done();
    img.onload = img.onerror = done;
  }))).then(() => {
    activate(bgIndex < 0 ? 0 : bgIndex);
    setInterval(() => activate((bgIndex + 1) % photos.length), BG_MS);
  });
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

function showCommitToast(commit) {
  const stack = dom.toastStack;
  if (!stack || !commit) return;

  const card = document.createElement('article');
  card.className = 'toast';
  card.innerHTML = '<span class="toast__label"></span>' +
                   '<div class="toast__name"></div>' +
                   '<div class="toast__desc"></div>';
  card.querySelector('.toast__label').textContent = 'NEW COMMIT';
  card.querySelector('.toast__name').textContent = commit.subject || 'Untitled commit';
  card.querySelector('.toast__desc').textContent =
    commit.description || 'No description in the commit body.';

  stack.prepend(card);
  while (stack.children.length > 3) stack.lastElementChild.remove();
  setTimeout(() => {
    card.classList.add('toast--out');
    setTimeout(() => card.remove(), 400);
  }, 9000);
}

function notifyCommits(commits) {
  if (!Array.isArray(commits) || !commits.length) return;
  if (lastCommitHash === null) {
    lastCommitHash = commits[0].hash;
    // Если эфир перезагрузили сразу после коммита, всё равно показываем его.
    const newest = commits[0];
    if (newest.timestamp && Date.now() / 1000 - newest.timestamp < 180) {
      showCommitToast(newest);
    }
    return;
  }
  const fresh = [];
  for (const commit of commits) {
    if (commit.hash === lastCommitHash) break;
    fresh.push(commit);
  }
  if (!fresh.length) return;
  lastCommitHash = commits[0].hash;
  fresh.reverse().forEach((commit, i) => {
    setTimeout(() => showCommitToast(commit), i * 350);
  });
}

function render(data) {
  const sc = data.progress.supercraft;
  const mcl = data.progress.mcpelauncher;
  const bugs = data.bugfix;

  // Hero — SuperCraft is the main subject of the stream.
  dom.heroRemain.innerHTML = `${r1(sc.remaining)}<span>%</span>`;
  dom.heroBar.style.width = `${clamp(sc.done).toFixed(2)}%`;
  dom.heroFoot.textContent = `${r1(sc.done)}% complete · ${sc.state.toLowerCase()}`;

  // Runner cards.
  setBar(dom.scBar, sc.done);
  setBar(dom.mclBar, mcl.done);
  dom.scPct.textContent = `${r1(sc.done)}%`;
  dom.mclPct.textContent = `${r1(mcl.done)}%`;
  dom.scHint.textContent = `${r1(sc.remaining)}% left · ${sc.state.toLowerCase()}`;
  dom.mclHint.textContent = `${r1(mcl.remaining)}% left · ${mcl.state.toLowerCase()}`;
  dom.scStatus.textContent = sc.remaining > 0 ? 'IN DEVELOPMENT' : 'COMPLETE';
  dom.mclStatus.textContent = mcl.remaining > 0 ? 'IN DEVELOPMENT' : 'COMPLETE';
  dom.scCard.classList.toggle('runner--final', sc.remaining === 0);
  dom.mclCard.classList.toggle('runner--final', mcl.remaining === 0);

  // Fixes.
  dom.fixPct.textContent = `${r1(bugs.percent)}%`;
  dom.fixCount.textContent = `${bugs.fixed} / ${bugs.total}`;
  setBar(dom.fixBar, bugs.percent);
  renderFixes(data.bugs);
  notifyCommits(data.supercraft && data.supercraft.commits);

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

setupBackdrop();
renderTrack();
setInterval(renderTrack, 7000);
refresh();
setInterval(refresh, POLL_MS);
