// WaveHopper — frontend entry.
// Steps 1-4: shell, MP3+HLS playback with auto-skip, config mode + localStorage.

const $ = (sel) => document.querySelector(sel);

const els = {
  list: $('#list'),
  play: $('#play'),
  next: $('#next'),
  status: $('#status'),
  modeToggle: $('#mode-toggle'),
  share: $('#share'),
  title: $('.bar h1'),
  listLabel: $('.list-wrap .list-label'),
  now: $('#now'),
  nowStation: $('.now-station'),
  nowChannel: $('.now-channel'),
  nowCity: $('.now-city'),
  controls: $('.controls'),
  skinRow: $('#skin-row'),
  meta: $('#now-meta'),
  metaTitle: $('#now-meta-title'),
  metaSub: $('#now-meta-sub'),
  aboutBtn: $('#about-btn'),
  aboutModal: $('#about-modal'),
  modalClose: $('#modal-close'),
  modalFooter: $('#modal-footer'),
};

const LS_KEY = 'wh:disabled';
const LS_SKIN = 'wh:skin';
const LS_LAST = 'wh:lastStationId';

const SKINS = [
  { id: 'dark',    label: 'Dark',    mono: false },
  { id: 'paper',   label: 'Paper',   mono: false },
  { id: 'fantasy', label: 'Fantasy', mono: true  },
  { id: 'winamp',  label: 'Winamp',  mono: true  },
];
const SKIN_IDS = new Set(SKINS.map((s) => s.id));
// Older builds wrote 'default' / 'green' / 'amber' here. Fold those into 'dark'.
const SKIN_MIGRATIONS = { default: 'dark', green: 'dark', amber: 'dark', clippy: 'winamp' };

// Bright 8-bit palette shared by Dark and Paper skins. Color is derived from
// station position so each station always gets the same accent.
// Excludes dark/grey tones to keep good contrast on both light and dark backgrounds.
const STATION_ACCENTS = [
  '#ff5555', '#55ccff', '#ffdd00', '#ff55ff', '#44dd44',
  '#ff8800', '#ff0080', '#00ddcc', '#9955ff', '#ffaa00',
];

const state = {
  stations: [],
  disabled: new Set(),   // station ids the user has switched off
  currentIndex: -1,
  playing: false,
  hls: null,
  autoSkipChain: 0,
  epoch: 0,
  mode: 'player',        // 'player' | 'config'
  skin: 'dark',
  attachedId: null,      // station id whose stream is currently attached to <audio>
};

// Track currentTime progression so we can detect a frozen audio element even when
// `audio.paused` lies (Safari, post-sleep). Declared up here to dodge TDZ — the
// stuck-time watcher resets these inside selectAndPlay, defined before them.
let lastTime = 0;
let lastTimeAt = 0;

// <video> instead of <audio> so hls.js can open a video/mp4 source buffer for
// HLS streams that carry a video track (e.g. Livepeer broadcasts). The media
// element API is identical — all existing event listeners and MediaSession calls
// work unchanged. Must be in the DOM for MSE to initialise on all browsers.
const audio = document.createElement('video');
audio.preload = 'none';
audio.setAttribute('playsinline', '');
audio.setAttribute('aria-hidden', 'true');
audio.style.cssText = 'position:fixed;width:0;height:0;opacity:0;pointer-events:none';
document.body.appendChild(audio);

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
function stationAccent() {
  return STATION_ACCENTS[state.currentIndex % STATION_ACCENTS.length];
}
function applyAccent() {
  // Monochrome skins keep their fixed CSS accent — clear any inline override.
  if (isMonoSkin()) {
    document.documentElement.style.removeProperty('--accent');
    return;
  }
  // Dark and Paper: accent derived from station position so it's stable and consistent.
  if (state.currentIndex >= 0) {
    document.documentElement.style.setProperty('--accent', stationAccent());
  }
}

function loadSkin() {
  try {
    const raw = localStorage.getItem(LS_SKIN);
    if (!raw) return;
    const migrated = SKIN_MIGRATIONS[raw];
    if (migrated) {
      state.skin = migrated;
      try { localStorage.setItem(LS_SKIN, migrated); } catch {}
      return;
    }
    if (SKIN_IDS.has(raw)) state.skin = raw;
  } catch {}
}

function saveLastStation(id) {
  try { localStorage.setItem(LS_LAST, id); } catch {}
}
function loadLastStation() {
  try { return localStorage.getItem(LS_LAST); } catch { return null; }
}
function applySkin() {
  document.body.dataset.skin = state.skin;
  // Re-evaluate accent: switching to/from mono skin must add or remove the inline override.
  applyAccent();
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
const PAUSE = '|| PAUSE';
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
  applyAccent();
  updateMediaSession(s);
}

function updateMediaSession(s) {
  if (!('mediaSession' in navigator)) return;
  const stationName = s.channel && s.channel !== 'main' ? `${s.station} · ${s.channel}` : s.station;
  const meta = (np.data && np.stationId === s.id) ? np.data : null;

  // Artist = station name, always (prominent line on Android lock screen / notification).
  // Title  = "Artist — Track" if metadata available, otherwise city fallback.
  let title;
  if (meta && meta.title) {
    title = meta.subtitle ? `${meta.subtitle} — ${meta.title}` : meta.title;
  } else {
    title = s.city || 'WaveHopper';
  }
  navigator.mediaSession.metadata = new MediaMetadata({
    title,
    artist: stationName,
    album: 'WaveHopper',
    artwork: [
      { src: '/img/favicon/android-chrome-192x192.png', sizes: '192x192', type: 'image/png' },
      { src: '/img/favicon/android-chrome-512x512.png', sizes: '512x512', type: 'image/png' },
    ],
  });
  navigator.mediaSession.playbackState = state.playing ? 'playing' : 'paused';
}

// === Now-playing metadata ===
// Server: PHP dispatcher at /api/now-playing.php?id=<id>. Stations declare their
// source via stations.json's `nowPlaying.type`. Polling runs only while we are
// playing AND the page is visible — anything else is a battery/data waste.

const NP_POLL_MS = 30000;
const np = {
  data: null,        // last successful payload from the dispatcher
  stationId: null,   // station id that payload belongs to
  ctrl: null,        // AbortController for the in-flight fetch
  timer: 0,          // setTimeout id for the next poll
};

function clearNowMeta() {
  np.data = null;
  np.stationId = null;
  els.metaTitle.textContent = '';
  els.metaSub.textContent = '';
}

function stopNowPlayingPoll({ clearUi = false } = {}) {
  if (np.ctrl) { try { np.ctrl.abort(); } catch {} np.ctrl = null; }
  if (np.timer) { clearTimeout(np.timer); np.timer = 0; }
  if (clearUi) clearNowMeta();
}

function hasServerSource(s) {
  // 'none' = explicitly no source; 'hls-id3' = client-side via HLS frag metadata,
  // not the PHP dispatcher. Both skip the polling path.
  if (!s || !s.nowPlaying || !s.nowPlaying.type) return false;
  const t = s.nowPlaying.type;
  return t !== 'none' && t !== 'hls-id3';
}

function renderNowMeta(s, data) {
  if (!data || !data.title) { clearNowMeta(); return; }
  els.metaTitle.textContent = data.title;
  let sub = data.subtitle || '';
  if (!sub && data.ends) {
    // No host/artist context — synthesize "until HH:MM" from the slot end time.
    const t = new Date(data.ends);
    if (!isNaN(t.getTime())) {
      const hh = String(t.getHours()).padStart(2, '0');
      const mm = String(t.getMinutes()).padStart(2, '0');
      sub = `until ${hh}:${mm}`;
    }
  }
  els.metaSub.textContent = sub;
  updateMediaSession(s);
}

async function fetchNowPlayingOnce(s) {
  if (!hasServerSource(s)) { clearNowMeta(); return; }
  if (np.ctrl) { try { np.ctrl.abort(); } catch {} }
  const ctrl = new AbortController();
  np.ctrl = ctrl;
  try {
    const res = await fetch(`/api/now-playing.php?id=${encodeURIComponent(s.id)}`, {
      signal: ctrl.signal,
      cache: 'no-cache',
    });
    if (ctrl.signal.aborted) return;
    // 204 = nothing playing or station has no source — wipe the card.
    if (res.status === 204) { clearNowMeta(); np.stationId = s.id; return; }
    if (!res.ok) return; // transient — keep whatever we already had
    const data = await res.json();
    if (ctrl.signal.aborted) return;
    // Race guard: user may have switched stations while we were waiting.
    if (state.stations[state.currentIndex] !== s) return;
    np.data = data;
    np.stationId = s.id;
    renderNowMeta(s, data);
  } catch (err) {
    if (err && err.name === 'AbortError') return;
    // Network blip — silently keep prior display.
  } finally {
    if (np.ctrl === ctrl) np.ctrl = null;
  }
}

function startNowPlayingPoll(s) {
  stopNowPlayingPoll();
  if (!hasServerSource(s)) { clearNowMeta(); return; }
  const tick = async () => {
    await fetchNowPlayingOnce(s);
    // Bail if the station changed under us, or polling was stopped.
    if (state.stations[state.currentIndex] !== s) return;
    if (!state.playing) return;
    np.timer = setTimeout(tick, NP_POLL_MS);
  };
  tick();
}

function setupMediaSessionActions() {
  if (!('mediaSession' in navigator)) return;
  const safe = (fn) => () => { try { fn(); } catch {} };
  navigator.mediaSession.setActionHandler('play', safe(userPlay));
  navigator.mediaSession.setActionHandler('pause', safe(() => audio.pause()));
  navigator.mediaSession.setActionHandler('nexttrack', safe(nextStation));
  navigator.mediaSession.setActionHandler('previoustrack', safe(prevStation));
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
  // Old attachment (if any) is being torn down — invalidate the marker now so a
  // user tap during the async setup window doesn't think we're already attached.
  state.attachedId = null;
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
  // New station: drop stale metadata + abort any in-flight fetch from the previous one.
  stopNowPlayingPoll({ clearUi: true });

  setStatus('connecting…');
  renderNow();
  renderList();

  try {
    await attachStream(s, myEpoch);
    if (myEpoch !== state.epoch) return;
    state.attachedId = s.id;
    await audio.play();
    if (myEpoch !== state.epoch) return;
    state.playing = true;
    state.autoSkipChain = 0;
    setStatus('on air');
    saveLastStation(s.id);
    startNowPlayingPoll(s);
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

function userPlay() {
  // Equivalent to "press PLAY" without the toggle behaviour. Used by the play
  // button (when not playing) and by Media Session 'play' so the lock-screen
  // play button works on a fresh load too.
  if (state.currentIndex < 0 || !isEnabled(state.stations[state.currentIndex])) {
    const first = nextEnabledFrom(-1);
    if (first >= 0) selectAndPlay(first, { userInitiated: true });
    return;
  }
  const cur = state.stations[state.currentIndex];
  if (state.attachedId !== cur.id) {
    // No stream attached for this station (fresh load with last-station memory,
    // or post-failure). Run the full attach + play path.
    selectAndPlay(state.currentIndex, { userInitiated: true });
    return;
  }
  audio.play().catch(() => setStatus('playback blocked', true));
}

function togglePlay() {
  if (state.playing) { audio.pause(); return; }
  userPlay();
}

function nextStation() {
  const next = nextEnabledFrom(state.currentIndex);
  if (next < 0) return;
  selectAndPlay(next, { userInitiated: true });
}

function prevStation() {
  if (!state.stations.length) return;
  for (let i = 1; i <= state.stations.length; i++) {
    const j = (state.currentIndex - i + state.stations.length) % state.stations.length;
    if (isEnabled(state.stations[j])) { selectAndPlay(j, { userInitiated: true }); return; }
  }
}

audio.addEventListener('play', () => { state.playing = true; renderNow(); });
audio.addEventListener('playing', () => {
  state.autoSkipChain = 0;
  setStatus('on air');
  // Resume polling if we were paused/backgrounded and just came back.
  if (!np.timer && state.currentIndex >= 0) {
    const s = state.stations[state.currentIndex];
    if (s) startNowPlayingPoll(s);
  }
});
audio.addEventListener('pause', () => {
  state.playing = false;
  stopNowPlayingPoll();
  renderNow();
  if (state.mode === 'player') setStatus('paused');
  if (swPendingReload) maybeReloadForSW();
});
audio.addEventListener('waiting', () => { if (state.mode === 'player') setStatus('buffering…'); });
audio.addEventListener('error', () => {
  if (state.hls) return;
  state.playing = false;
  autoSkipOnFailure('stream error');
});

els.play.addEventListener('click', togglePlay);
els.next.addEventListener('click', nextStation);
els.modeToggle.addEventListener('click', () => setMode(state.mode === 'config' ? 'player' : 'config'));
els.share.addEventListener('click', shareApp);
els.aboutBtn.addEventListener('click', openAbout);
els.modalClose.addEventListener('click', closeAbout);
els.aboutModal.addEventListener('click', (e) => { if (e.target === els.aboutModal) closeAbout(); });
document.addEventListener('keydown', (e) => { if (e.key === 'Escape') closeAbout(); });

function openAbout() {
  const total = state.stations.length;
  els.modalFooter.textContent = total
    ? `${total} stations · ${enabledCount()} enabled`
    : '';
  els.aboutModal.hidden = false;
}
function closeAbout() {
  els.aboutModal.hidden = true;
}

async function shareApp() {
  const url = location.origin + '/';
  // navigator.share is mobile-first; requires a user gesture and HTTPS.
  if (navigator.share) {
    try {
      await navigator.share({ title: 'WaveHopper', text: 'WaveHopper', url });
    } catch (err) {
      // AbortError = user dismissed the sheet; anything else falls through to clipboard.
      if (err && err.name === 'AbortError') return;
      copyToClipboard(url);
    }
    return;
  }
  copyToClipboard(url);
}

async function copyToClipboard(url) {
  try {
    if (navigator.clipboard && navigator.clipboard.writeText) {
      await navigator.clipboard.writeText(url);
    } else {
      // Pre-Clipboard-API fallback (older Safari over plain HTTP, etc.).
      const ta = document.createElement('textarea');
      ta.value = url;
      ta.setAttribute('readonly', '');
      ta.style.position = 'fixed';
      ta.style.left = '-9999px';
      document.body.appendChild(ta);
      ta.select();
      document.execCommand('copy');
      document.body.removeChild(ta);
    }
    setStatus('link copied');
  } catch {
    setStatus('share unavailable', true);
  }
}

// Swipe left → NEXT, swipe right → PREV on the now-playing card. Touch only.
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
    // Visual nudge in both directions, capped at ±80px.
    dragX = Math.max(-80, Math.min(80, dx));
    els.now.style.transform = `translateX(${dragX}px)`;
  }, { passive: true });

  function end() {
    if (!tracking) return;
    tracking = false;
    els.now.style.transition = 'transform 140ms ease-out';
    els.now.style.transform = RESET;
    if (dragX <= -55) nextStation();
    else if (dragX >= 55) prevStation();
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
  if (document.visibilityState !== 'visible') {
    // Hidden: pause polling. Audio keeps going via the OS; metadata can wait.
    stopNowPlayingPoll();
    return;
  }
  if (!state.playing) return;
  // Phone wake-up / long sleep can leave the audio element silently dead even when
  // not technically paused. Treat 'paused' OR 'currentTime stuck >5s' as stale.
  if (audio.paused) {
    audio.play().catch(() => reattachCurrent('reconnecting…'));
  } else if (isStuck(5000)) {
    reattachCurrent('reconnecting…');
  }
  // Coming back foreground while playing: refresh metadata immediately.
  const s = state.stations[state.currentIndex];
  if (s) startNowPlayingPoll(s);
});

window.addEventListener('online', () => {
  if (state.currentIndex < 0) return;
  if (state.playing && (audio.paused || isStuck(3000))) reattachCurrent('reconnecting…');
});

window.addEventListener('offline', () => {
  if (state.currentIndex >= 0) setStatus('offline — waiting for network', true);
});

// === Service worker registration + auto-update ===
// On a fresh deploy: the browser fetches the new sw.js, installs it (which
// pre-caches the new shell), then waits. We tell it to skipWaiting so the new
// SW activates and claims this page. controllerchange fires, and we reload —
// but only if not playing, since reload kills mid-stream audio. If playing, we
// flag a pending reload and trigger it the next time the user pauses.
let swPendingReload = false;

function maybeReloadForSW() {
  if (sessionStorage.getItem('wh:reloaded-for-sw')) return;
  sessionStorage.setItem('wh:reloaded-for-sw', '1');
  location.reload();
}

function registerServiceWorker() {
  if (!('serviceWorker' in navigator)) return;
  navigator.serviceWorker.register('/sw.js').then((reg) => {
    // A waiting SW already present at registration time (race on first paint).
    if (reg.waiting) reg.waiting.postMessage('skipWaiting');
    reg.addEventListener('updatefound', () => {
      const next = reg.installing;
      if (!next) return;
      next.addEventListener('statechange', () => {
        if (next.state === 'installed' && navigator.serviceWorker.controller) {
          // New version ready and an old SW is currently in control — trigger handoff.
          next.postMessage('skipWaiting');
        }
      });
    });
  }).catch(() => {});

  navigator.serviceWorker.addEventListener('controllerchange', () => {
    if (state.playing) {
      swPendingReload = true;
      setStatus('update ready — pause to reload');
    } else {
      maybeReloadForSW();
    }
  });
}

async function init() {
  loadSkin();
  applySkin();
  renderSkinRow();
  const hadSavedDisabled = loadDisabled();
  setupMediaSessionActions();
  registerServiceWorker();
  try {
    const res = await fetch('/stations.json', { cache: 'no-cache' });
    state.stations = await res.json();
  } catch {
    setStatus('failed to load stations', true);
    return;
  }
  if (!hadSavedDisabled) seedDefaultDisabled();

  // Pre-select last-played station so PLAY resumes the right one. We don't autoplay —
  // browsers block audio without a user gesture, and silent failure is worse than
  // requiring one tap.
  const lastId = loadLastStation();
  if (lastId) {
    const idx = state.stations.findIndex((s) => s.id === lastId && isEnabled(s));
    if (idx >= 0) state.currentIndex = idx;
  }

  renderList();
  renderNow();
  if (state.currentIndex >= 0) {
    const s = state.stations[state.currentIndex];
    const name = s.channel && s.channel !== 'main' ? `${s.station} · ${s.channel}` : s.station;
    setStatus(`tap PLAY for ${name}`);
  } else {
    setStatus(`${enabledCount()} stations`);
  }
}

init();
