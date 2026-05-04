// WaveHopper — frontend entry.
// Steps 1-4: shell, MP3+HLS playback with auto-skip, config mode + localStorage.

const $ = (sel) => document.querySelector(sel);

const els = {
  list: $('#list'),
  play: $('#play'),
  next: $('#next'),
  status: $('#status'),
  modeToggle: $('#mode-toggle'),
  title: $('.bar h1'),
  listLabel: $('.list-label'),
  now: $('#now'),
  nowStation: $('.now-station'),
  nowChannel: $('.now-channel'),
  nowCity: $('.now-city'),
  controls: $('.controls'),
  skinRow: $('#skin-row'),
};

const LS_KEY = 'wh:disabled';
const LS_SKIN = 'wh:skin';

const SKINS = [
  { id: 'default', label: 'Default', mono: false },
  { id: 'green',   label: 'Green',   mono: true  },
  { id: 'amber',   label: 'Amber',   mono: true  },
  { id: 'paper',   label: 'Paper',   mono: false },
];
const SKIN_IDS = new Set(SKINS.map((s) => s.id));

const state = {
  stations: [],
  disabled: new Set(),   // station ids the user has switched off
  currentIndex: -1,
  playing: false,
  hls: null,
  autoSkipChain: 0,
  epoch: 0,
  mode: 'player',        // 'player' | 'config'
  skin: 'default',
};

// Track currentTime progression so we can detect a frozen audio element even when
// `audio.paused` lies (Safari, post-sleep). Declared up here to dodge TDZ — the
// stuck-time watcher resets these inside selectAndPlay, defined before them.
let lastTime = 0;
let lastTimeAt = 0;

const audio = new Audio();
audio.preload = 'none';
audio.setAttribute('playsinline', '');

let hlsLoader = null;
function loadHlsLib() {
  if (window.Hls) return Promise.resolve(window.Hls);
  if (hlsLoader) return hlsLoader;
  hlsLoader = new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.src = '/vendor/hls.light.min.js';
    s.async = true;
    s.onload = () => resolve(window.Hls);
    s.onerror = () => reject(new Error('hls.js failed to load'));
    document.head.appendChild(s);
  });
  return hlsLoader;
}

const isHls = (s) => s.format === 'hls' || /\.m3u8(\?|$)/i.test(s.url);
const nativeHls = audio.canPlayType('application/vnd.apple.mpegurl') !== '';
const isEnabled = (s) => !state.disabled.has(s.id);

function loadDisabled() {
  try {
    const raw = localStorage.getItem(LS_KEY);
    if (raw) {
      state.disabled = new Set(JSON.parse(raw));
      return true;
    }
  } catch {}
  return false;
}
function seedDefaultDisabled() {
  for (const s of state.stations) {
    if (s.defaultDisabled) state.disabled.add(s.id);
  }
  saveDisabled();
}
function saveDisabled() {
  try {
    localStorage.setItem(LS_KEY, JSON.stringify([...state.disabled]));
  } catch {}
}

function enabledCount() {
  let n = 0;
  for (const s of state.stations) if (isEnabled(s)) n++;
  return n;
}
function nextEnabledFrom(idx) {
  if (!state.stations.length) return -1;
  for (let i = 1; i <= state.stations.length; i++) {
    const j = (idx + i + state.stations.length) % state.stations.length;
    if (isEnabled(state.stations[j])) return j;
  }
  return -1;
}

function setStatus(text, isError = false) {
  els.status.textContent = text;
  els.status.classList.toggle('error', !!isError);
}

function isMonoSkin() {
  const s = SKINS.find((x) => x.id === state.skin);
  return !!(s && s.mono);
}
function applyAccent(color) {
  // Monochrome skins (green/amber phosphor) keep their fixed accent — let the skin's
  // CSS variable win by clearing any inline override JS may have set previously.
  if (isMonoSkin()) {
    document.documentElement.style.removeProperty('--accent');
    return;
  }
  if (color) document.documentElement.style.setProperty('--accent', color);
}

function loadSkin() {
  try {
    const raw = localStorage.getItem(LS_SKIN);
    if (raw && SKIN_IDS.has(raw)) state.skin = raw;
  } catch {}
}
function applySkin() {
  document.body.dataset.skin = state.skin;
  // Re-evaluate accent: switching to/from mono skin must add or remove the inline override.
  const s = state.stations[state.currentIndex];
  applyAccent(s ? s.color : null);
}
function setSkin(id) {
  if (!SKIN_IDS.has(id) || state.skin === id) return;
  state.skin = id;
  try { localStorage.setItem(LS_SKIN, id); } catch {}
  applySkin();
  renderSkinRow();
}
function renderSkinRow() {
  if (!els.skinRow) return;
  els.skinRow.innerHTML = '';
  for (const s of SKINS) {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'skin-btn';
    b.textContent = s.label;
    b.setAttribute('role', 'radio');
    b.setAttribute('aria-pressed', state.skin === s.id ? 'true' : 'false');
    b.addEventListener('click', () => setSkin(s.id));
    els.skinRow.appendChild(b);
  }
}

function teardownHls() {
  if (state.hls) {
    try { state.hls.destroy(); } catch {}
    state.hls = null;
  }
}

// Symbols with emoji presentation defaults on Apple platforms — append U+FE0E
// (variation selector-15) to force the text-style glyph instead of color emoji.
const PLAY  = '▶︎ PLAY';
const PAUSE = '⏸︎ PAUSE';
const GEAR  = '⚙︎';
const CHECK = '✓';

function renderNow() {
  const s = state.stations[state.currentIndex];
  if (!s) {
    els.nowStation.textContent = '—';
    els.nowChannel.textContent = '';
    els.nowCity.textContent = '';
    els.play.textContent = PLAY;
    return;
  }
  els.nowStation.textContent = s.station;
  els.nowChannel.textContent = s.channel === 'main' ? '' : s.channel;
  els.nowCity.textContent = s.city || '';
  els.play.textContent = state.playing ? PAUSE : PLAY;
  applyAccent(s.color);
  updateMediaSession(s);
}

function updateMediaSession(s) {
  if (!('mediaSession' in navigator)) return;
  const title = s.channel && s.channel !== 'main' ? `${s.station} · ${s.channel}` : s.station;
  navigator.mediaSession.metadata = new MediaMetadata({
    title,
    artist: s.city || 'WaveHopper',
    album: 'WaveHopper',
    artwork: [
      { src: '/icons/icon-192.png', sizes: '192x192', type: 'image/png' },
      { src: '/icons/icon-512.png', sizes: '512x512', type: 'image/png' },
    ],
  });
  navigator.mediaSession.playbackState = state.playing ? 'playing' : 'paused';
}

function setupMediaSessionActions() {
  if (!('mediaSession' in navigator)) return;
  const safe = (fn) => () => { try { fn(); } catch {} };
  navigator.mediaSession.setActionHandler('play', safe(() => {
    audio.play().catch(() => {});
  }));
  navigator.mediaSession.setActionHandler('pause', safe(() => audio.pause()));
  navigator.mediaSession.setActionHandler('nexttrack', safe(nextStation));
  navigator.mediaSession.setActionHandler('previoustrack', safe(() => {
    // "previous" = previous enabled station (cyclic), since radio has no seek.
    if (!state.stations.length) return;
    for (let i = 1; i <= state.stations.length; i++) {
      const j = (state.currentIndex - i + state.stations.length) % state.stations.length;
      if (isEnabled(state.stations[j])) { selectAndPlay(j, { userInitiated: true }); return; }
    }
  }));
  // Live radio has no seekable timeline — explicitly null these so the OS hides the scrubber.
  for (const a of ['seekto', 'seekbackward', 'seekforward', 'stop']) {
    try { navigator.mediaSession.setActionHandler(a, null); } catch {}
  }
}

function renderList() {
  els.list.innerHTML = '';

  const items = state.mode === 'config'
    ? state.stations.map((s, i) => ({ s, i }))
    : state.stations.map((s, i) => ({ s, i })).filter(({ s }) => isEnabled(s));

  for (const { s, i } of items) {
    const li = document.createElement('li');
    const active = i === state.currentIndex && state.mode === 'player';
    li.className = 'row' + (active ? ' active' : '') + (state.mode === 'config' ? ' row-config' : '');
    li.dataset.index = String(i);

    const dot = document.createElement('span');
    if (state.mode === 'config') {
      dot.className = 'row-check' + (isEnabled(s) ? ' on' : '');
      dot.textContent = isEnabled(s) ? '☒' : '☐';
    } else {
      dot.className = 'row-dot';
    }
    li.appendChild(dot);

    const name = document.createElement('span');
    name.className = 'row-name';
    name.textContent = s.station;
    li.appendChild(name);

    if (s.channel && s.channel !== 'main') {
      const ch = document.createElement('span');
      ch.className = 'row-channel';
      ch.textContent = s.channel;
      li.appendChild(ch);
    }

    if (state.mode === 'player' && s.city) {
      const city = document.createElement('span');
      city.className = 'row-city';
      city.textContent = s.city;
      li.appendChild(city);
    }

    li.addEventListener('click', () => {
      if (state.mode === 'config') toggleStation(i);
      else selectAndPlay(i, { userInitiated: true });
    });
    els.list.appendChild(li);
  }

  if (state.mode === 'player' && items.length === 0) {
    const li = document.createElement('li');
    li.className = 'row empty';
    li.textContent = `no stations enabled — tap ${GEAR} to add some`;
    els.list.appendChild(li);
  }
}

function toggleStation(i) {
  const s = state.stations[i];
  if (!s) return;
  if (state.disabled.has(s.id)) {
    state.disabled.delete(s.id);
  } else {
    if (enabledCount() <= 1) {
      // Block disabling the last enabled station — keeps NEXT/PLAY meaningful.
      setStatus('keep at least one station enabled', true);
      return;
    }
    state.disabled.add(s.id);
  }
  saveDisabled();
  setStatus(`${enabledCount()} of ${state.stations.length} enabled`);
  renderList();
}

function setMode(mode) {
  state.mode = mode;
  document.body.classList.toggle('config-mode', mode === 'config');
  els.title.textContent = mode === 'config' ? 'CONFIG' : 'WAVEHOPPER';
  els.modeToggle.textContent = mode === 'config' ? CHECK : GEAR;
  els.modeToggle.setAttribute('aria-label', mode === 'config' ? 'Done' : 'Settings');
  els.listLabel.textContent = mode === 'config' ? 'ENABLE / DISABLE' : 'STATIONS';
  if (mode === 'config') {
    setStatus(`${enabledCount()} of ${state.stations.length} enabled`);
  } else {
    setStatus(state.playing ? 'on air' : '');
  }
  renderList();
}

async function attachStream(s, epoch) {
  teardownHls();
  audio.removeAttribute('src');
  audio.load();

  if (isHls(s) && !nativeHls) {
    const Hls = await loadHlsLib();
    if (epoch !== state.epoch) return;
    if (!Hls.isSupported()) throw new Error('HLS not supported');
    const hls = new Hls({
      maxBufferLength: 12,
      maxMaxBufferLength: 30,
      liveSyncDurationCount: 3,
    });
    state.hls = hls;
    hls.attachMedia(audio);
    await new Promise((resolve) => hls.on(Hls.Events.MEDIA_ATTACHED, resolve));
    if (epoch !== state.epoch) return;
    hls.loadSource(s.url);

    // Soft-recovery: on fatal NETWORK_ERROR try startLoad() once, on fatal MEDIA_ERROR
    // try recoverMediaError() once, before declaring the station off-air. Avoids
    // skipping when the upstream just hiccuped.
    let netRecovered = false, mediaRecovered = false;
    hls.on(Hls.Events.ERROR, (_evt, data) => {
      if (epoch !== state.epoch) return;
      if (!data.fatal) return;
      if (data.type === Hls.ErrorTypes.NETWORK_ERROR && !netRecovered) {
        netRecovered = true;
        try { hls.startLoad(); return; } catch {}
      }
      if (data.type === Hls.ErrorTypes.MEDIA_ERROR && !mediaRecovered) {
        mediaRecovered = true;
        try { hls.recoverMediaError(); return; } catch {}
      }
      autoSkipOnFailure('off air');
    });
  } else {
    audio.src = s.url;
  }
}

async function selectAndPlay(index, { userInitiated = false } = {}) {
  if (index < 0 || index >= state.stations.length) return;
  if (userInitiated) state.autoSkipChain = 0;
  const myEpoch = ++state.epoch;
  state.currentIndex = index;
  const s = state.stations[index];

  // Reset stuck-time tracker — the next 'playing' event will rearm it.
  lastTimeAt = 0;

  setStatus('connecting…');
  renderNow();
  renderList();

  try {
    await attachStream(s, myEpoch);
    if (myEpoch !== state.epoch) return;
    await audio.play();
    if (myEpoch !== state.epoch) return;
    state.playing = true;
    state.autoSkipChain = 0;
    setStatus('on air');
  } catch (err) {
    if (myEpoch !== state.epoch) return;
    if (err && err.name === 'AbortError') return;
    state.playing = false;
    if (userInitiated && err && err.name === 'NotAllowedError') {
      setStatus('tap PLAY to start');
    } else {
      autoSkipOnFailure('stream error');
    }
  }
  renderNow();
}

function autoSkipOnFailure(reason) {
  state.autoSkipChain += 1;
  const cap = Math.max(1, enabledCount());
  if (state.autoSkipChain >= cap) {
    setStatus('no stations on air', true);
    state.playing = false;
    teardownHls();
    renderNow();
    return;
  }
  const next = nextEnabledFrom(state.currentIndex);
  if (next < 0) {
    setStatus('no stations on air', true);
    return;
  }
  setStatus(`${reason} — skipping…`);
  setTimeout(() => selectAndPlay(next, { userInitiated: false }), 600);
}

function togglePlay() {
  if (state.currentIndex < 0 || !isEnabled(state.stations[state.currentIndex])) {
    const first = nextEnabledFrom(-1);
    if (first >= 0) selectAndPlay(first, { userInitiated: true });
    return;
  }
  if (state.playing) {
    audio.pause();
  } else {
    audio.play().catch(() => setStatus('playback blocked', true));
  }
}

function nextStation() {
  const next = nextEnabledFrom(state.currentIndex);
  if (next < 0) return;
  selectAndPlay(next, { userInitiated: true });
}

audio.addEventListener('play', () => { state.playing = true; renderNow(); });
audio.addEventListener('playing', () => { state.autoSkipChain = 0; setStatus('on air'); });
audio.addEventListener('pause', () => { state.playing = false; renderNow(); if (state.mode === 'player') setStatus('paused'); });
audio.addEventListener('waiting', () => { if (state.mode === 'player') setStatus('buffering…'); });
audio.addEventListener('error', () => {
  if (state.hls) return;
  state.playing = false;
  autoSkipOnFailure('stream error');
});

els.play.addEventListener('click', togglePlay);
els.next.addEventListener('click', nextStation);
els.modeToggle.addEventListener('click', () => setMode(state.mode === 'config' ? 'player' : 'config'));

// Swipe-left on the now-playing card → NEXT. Touch only — desktop has the button.
(function attachSwipe() {
  if (!els.now) return;
  let startX = 0, startY = 0, tracking = false, dragX = 0;
  const RESET = 'translateX(0)';

  els.now.addEventListener('touchstart', (e) => {
    if (state.mode !== 'player') return;
    if (e.touches.length !== 1) { tracking = false; return; }
    const t = e.touches[0];
    startX = t.clientX; startY = t.clientY; dragX = 0; tracking = true;
    els.now.style.transition = 'none';
  }, { passive: true });

  els.now.addEventListener('touchmove', (e) => {
    if (!tracking) return;
    const t = e.touches[0];
    const dx = t.clientX - startX;
    const dy = t.clientY - startY;
    // Vertical-dominant motion is a scroll attempt, not a swipe — bail.
    if (Math.abs(dy) > Math.abs(dx) && Math.abs(dy) > 10) { tracking = false; els.now.style.transform = RESET; return; }
    // Visual nudge only on left drag, capped.
    dragX = Math.max(-80, Math.min(0, dx));
    els.now.style.transform = `translateX(${dragX}px)`;
  }, { passive: true });

  function end() {
    if (!tracking) return;
    tracking = false;
    els.now.style.transition = 'transform 140ms ease-out';
    els.now.style.transform = RESET;
    if (dragX <= -55) nextStation();
  }
  els.now.addEventListener('touchend', end);
  els.now.addEventListener('touchcancel', end);
})();

// === Background / network hardening ===
// We never pause on visibility change — the OS handles backgrounding.
// We only react when coming back foreground or back online.

let reattaching = false;
function reattachCurrent(reason) {
  if (reattaching || state.currentIndex < 0) return;
  reattaching = true;
  setStatus(reason);
  selectAndPlay(state.currentIndex, { userInitiated: false })
    .finally(() => { reattaching = false; });
}

audio.addEventListener('timeupdate', () => {
  if (audio.currentTime !== lastTime) {
    lastTime = audio.currentTime;
    lastTimeAt = Date.now();
  }
});
audio.addEventListener('playing', () => { lastTime = audio.currentTime; lastTimeAt = Date.now(); });

function isStuck(thresholdMs) {
  // Fresh attachment: no timeupdate yet — give it room.
  if (lastTimeAt === 0) return false;
  return Date.now() - lastTimeAt > thresholdMs;
}

// Stuck-time watcher: if we think we're playing but currentTime hasn't advanced for
// 8s, the connection has died silently (common on mobile after a NAT/wifi blip).
// Reattach without skipping stations.
setInterval(() => {
  if (!state.playing || reattaching) return;
  if (document.visibilityState === 'hidden') return; // OS may legitimately throttle
  if (audio.paused) return; // pause path is handled elsewhere
  if (isStuck(8000)) reattachCurrent('reconnecting…');
}, 2000);

document.addEventListener('visibilitychange', () => {
  if (document.visibilityState !== 'visible') return;
  if (!state.playing) return;
  // Phone wake-up / long sleep can leave the audio element silently dead even when
  // not technically paused. Treat 'paused' OR 'currentTime stuck >5s' as stale.
  if (audio.paused) {
    audio.play().catch(() => reattachCurrent('reconnecting…'));
  } else if (isStuck(5000)) {
    reattachCurrent('reconnecting…');
  }
});

window.addEventListener('online', () => {
  if (state.currentIndex < 0) return;
  if (state.playing && (audio.paused || isStuck(3000))) reattachCurrent('reconnecting…');
});

window.addEventListener('offline', () => {
  if (state.currentIndex >= 0) setStatus('offline — waiting for network', true);
});

async function init() {
  loadSkin();
  applySkin();
  renderSkinRow();
  const hadSavedDisabled = loadDisabled();
  setupMediaSessionActions();
  if ('serviceWorker' in navigator) {
    navigator.serviceWorker.register('/sw.js').catch(() => {});
  }
  try {
    const res = await fetch('/stations.json', { cache: 'no-cache' });
    state.stations = await res.json();
  } catch {
    setStatus('failed to load stations', true);
    return;
  }
  if (!hadSavedDisabled) seedDefaultDisabled();
  renderList();
  renderNow();
  setStatus(`${enabledCount()} stations`);
}

init();
