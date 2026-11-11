// kepyur web: the page. The radio and decoder run in worker.js (kepyur.wasm, native/radio); this file is the
// start card, the video canvas, the OSD pill, the corner buttons, the settings and guide dialogs, ?debug.
import { BUILD } from './build.js';

const $ = (id) => document.getElementById(id);
const DEBUG = new URLSearchParams(location.search).has('debug');
const ADAPTERS = [{ vendorId: 0x2357, productId: 0x0115 }, { vendorId: 0x0bda, productId: 0xb812 }]; // T4U v3, 8812BU
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const now = () => performance.timeOrigin + performance.now(); // epoch ms, the clock the wasm side stamps with

// ---- remembered on this machine (best effort) ----
const store = {
  get(k, d) {
    try {
      const v = localStorage.getItem(`kepyur.${k}`);
      return v == null ? d : JSON.parse(v);
    } catch {
      return d;
    }
  },
  set(k, v) {
    try {
      localStorage.setItem(`kepyur.${k}`, JSON.stringify(v));
    } catch {}
  },
};

// ---- standard analog channels (native/dsp/spectrum.cpp fpv_channels / nearest_channel) ----
const CHANNELS = {
  A: [5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725], B: [5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866],
  E: [5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945], F: [5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880],
  R: [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917], L: [5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621],
};
function channelName(mhz) {
  let best = '', bd = 6;
  for (const band of 'RFABEL') // Raceband first on ties
    CHANNELS[band].forEach((f, i) => {
      if (Math.abs(f - mhz) < bd - 0.01) {
        bd = Math.abs(f - mhz);
        best = band + (i + 1);
      }
    });
  return best;
}

// ---- the worker ----
const worker = new Worker('./worker.js', { type: 'module' });
let moduleReady = false;
const waiting = new Map(); // command name -> resolve(reply)
function cmd(text) {
  const op = text.split(' ')[0];
  log(`> ${text}`);
  return new Promise((resolve) => {
    waiting.set(op, resolve);
    worker.postMessage({ t: 'cmd', text });
  });
}
worker.onmessage = ({ data: m }) => {
  if (m.t === 'frame') onFrame(m);
  else if (m.t === 'ev') {
    const e = JSON.parse(m.json);
    if (e.ev === 'status') onStatus(e);
    else if (e.ev === 'reply') {
      log(`< ${e.cmd}: ${e.text}`);
      waiting.get(e.cmd)?.(e);
      waiting.delete(e.cmd);
    }
  } else if (m.t === 'log') log(m.text);
  else if (m.t === 'ready') moduleReady = true;
  else if (m.t === 'fatal') fail(`The receiver could not start in this browser (${m.msg}).`);
};
async function untilReady() {
  for (let i = 0; i < 300 && !moduleReady; i++) await sleep(100);
  if (!moduleReady) throw new Error('the receiver did not start');
}

// ---- video ----
const stage = $('stage');
const canvas = $('video');
const ctx = canvas.getContext('2d', { alpha: false, desynchronized: true });
let pendingFrame = null, lastFrameAt = 0, latencies = [], latencyMs = null, painted = 0;
function onFrame(m) {
  if (!pendingFrame) requestAnimationFrame(paint);
  pendingFrame = m;
}
function paint() {
  const m = pendingFrame;
  pendingFrame = null;
  try {
    if (canvas.width !== m.w || canvas.height !== m.h) {
      canvas.width = m.w;
      canvas.height = m.h;
    }
    ctx.putImageData(new ImageData(new Uint8ClampedArray(m.buf), m.w, m.h), 0, 0);
    latencies.push(now() - m.ts);
    lastFrameAt = performance.now();
    painted++;
  } finally {
    worker.postMessage({ t: 'ack' });
  }
}
// a frozen last frame must never pass for live video
setInterval(() => {
  const stale = phase === 'watching' && performance.now() - lastFrameAt > 500;
  stage.classList.toggle('stale', stale);
  $('nopic').hidden = !stale;
  if (stale) $('nopic').textContent = last?.mode === 'scan' ? 'Scanning…' : 'No picture';
}, 100);

// ---- phases: start card -> connecting -> scanning -> watching ----
let phase = 'start';
let switching = false; // the USB 3 switch: the adapter disconnects on purpose
let scanFrom = null;   // 'card' | 'settings' while a scan runs
let last = null;       // the latest status
function setPhase(p) {
  phase = p;
  const watching = p === 'watching';
  stage.classList.toggle('watching', watching);
  $('card').hidden = watching;
  $('gear').hidden = !watching;
  showOsd(watching && !store.get('osdHidden', false));
  if (!watching) $('osd-show').hidden = true;
}
function card(text, { busy = false, connect = false, rescan = false, help = connect } = {}) {
  setPhase(phase === 'watching' ? 'start' : phase);
  $('card').hidden = false;
  $('card-text').textContent = text;
  $('connect').hidden = !connect;
  $('rescan').hidden = !rescan;
  $('card-help').hidden = !help;
  $('card-progress').hidden = !busy;
}
function progress(el, frac, text) {
  el.hidden = false;
  el.firstElementChild.style.width = `${Math.round(frac * 100)}%`;
  el.lastElementChild.textContent = text;
}
function fail(text, { guide = false } = {}) {
  phase = 'start';
  setPhase('start');
  card(text, { connect: !!navigator.usb, help: true });
  if (guide) $('guide-link').textContent = 'Set up the driver (Windows)';
}

async function pairedAdapters() {
  const ds = navigator.usb ? await navigator.usb.getDevices() : [];
  return ds.filter((d) => ADAPTERS.some((a) => a.vendorId === d.vendorId && a.productId === d.productId));
}
function waitForAdapter(ms) {
  return new Promise((resolve) => {
    const t = setTimeout(() => done(false), ms);
    const on = (e) => ADAPTERS.some((a) => a.productId === e.device.productId) && done(true);
    function done(v) {
      clearTimeout(t);
      navigator.usb.removeEventListener('connect', on);
      resolve(v);
    }
    navigator.usb.addEventListener('connect', on);
  });
}

async function connect(fromClick) {
  try {
    if (!(await pairedAdapters()).length) {
      if (!fromClick) return;
      try {
        await navigator.usb.requestDevice({ filters: ADAPTERS });
      } catch (e) {
        if (e.name === 'NotFoundError') {
          phase = 'start';
          return card('No adapter chosen. Plug in the T4U and click Connect.', { connect: true });
        }
        throw e;
      }
    }
    phase = 'connecting';
    card('Starting the receiver…', { busy: true });
    progress($('card-progress'), 0.1, '');
    await untilReady();
    switching = true;
    const sw = await cmd('usb3');
    if (sw.text.startsWith('Requested')) {
      card('Switching the adapter to USB 3…', { busy: true });
      progress($('card-progress'), 0.25, '');
      const back = await waitForAdapter(8000);
      if (!back || !(await pairedAdapters()).length) {
        switching = false;
        phase = 'start';
        return card('The adapter restarted in USB 3 mode. Click Connect and choose it again.', { connect: true });
      }
      await sleep(300);
    }
    switching = false;
    card('Starting the radio…', { busy: true });
    progress($('card-progress'), 0.45, '');
    const op = await cmd('open');
    if (!op.ok) {
      const access = /access|claim|busy|LIBUSB_ERROR|permission|denied/i.test(op.text);
      return fail(access
        ? 'Could not open the adapter. Close other programs or tabs using it. On Windows, set up the driver first.'
        : `Could not start the adapter: ${op.text}`, { guide: access });
    }
    if (op.text === 'USB 2') {
      await cmd('close');
      return fail('The adapter is running at USB 2, which is too slow. Plug it into a USB 3 port (usually blue) and replug it.');
    }
    startScan('card', store.get('lastMhz', 0));
  } catch (e) {
    switching = false;
    fail(`Could not connect: ${e.message ?? e}`);
  }
}

function startScan(from, preferMhz = 0) {
  scanFrom = from;
  if (from === 'card') {
    phase = 'scanning';
    card('Scanning for video…', { busy: true });
  }
  $('scan').disabled = true;
  cmd(preferMhz ? `scan ${preferMhz}` : 'scan');
}

// ---- status (2 Hz) ----
let prevMode = null, snrShown = null;
function onStatus(e) {
  const s = e.st;
  last = { ...s, t: e.t, sent: e.sent, skipped: e.skipped };
  if (latencies.length) {
    latencies.sort((a, b) => a - b);
    latencyMs = latencies[latencies.length >> 1];
    latencies = [];
  } else if (performance.now() - lastFrameAt > 1000) latencyMs = null;
  renderDebug();

  if (phase === 'start' || phase === 'connecting') return (prevMode = s.mode);
  if (s.rx_state === 'lost' || s.rx_state === 'error') {
    cmd('close');
    return fail(s.rx_state === 'lost' ? 'The adapter was unplugged. Plug it back in and click Connect.'
      : `The adapter stopped: ${s.rx_error}. Click Connect to start again.`);
  }
  if (s.mode === 'scan') {
    const frac = s.scan_steps ? s.scan_step / s.scan_steps : 0;
    const text = `${Math.round(s.freq_hz / 1e6)} MHz · ${s.scan_step} / ${s.scan_steps}`;
    progress(scanFrom === 'settings' ? $('scan-progress') : $('card-progress'), frac, text);
  } else if (prevMode === 'scan') {
    // a scan ended: watching (the preferred / previous / strongest pilot) or nothing found
    $('scan').disabled = false;
    $('scan-progress').hidden = true;
    $('pilots-note').textContent = s.pilots.length
      ? `Found ${s.pilots.length} · just now. Click one to watch it.`
      : 'No analog video found. Check the VTX is on, then scan again.';
    if (s.mode !== 'watch' && phase !== 'watching')
      card('No analog video found. Check that the VTX is powered, then scan again.', { rescan: true });
    scanFrom = null;
  }
  if (s.mode === 'watch') {
    if (phase !== 'watching') setPhase('watching');
    const mhz = s.watch_hz / 1e6;
    store.set('lastMhz', Math.round(mhz * 10) / 10);
    $('osd-ch').textContent = channelName(mhz);
    $('osd-mhz').textContent = `${Math.round(mhz)} MHz`;
    const snr = $('osd-snr');
    if (s.snr_db == null) {
      snrShown = null;
      snr.textContent = '–';
      snr.className = '';
    } else {
      snrShown = snrShown == null ? s.snr_db : 0.5 * snrShown + 0.5 * s.snr_db; // ~1 s smoothing
      snr.textContent = Math.round(snrShown);
      snr.className = snrShown >= 25 ? 'good' : snrShown >= 15 ? 'mid' : 'bad';
    }
    $('osd-lat').textContent = latencyMs == null ? '– ms' : `${Math.round(latencyMs)} ms`;
  }
  renderPilots(s);
  prevMode = s.mode;
}

// ---- settings: Channels ----
function renderPilots(s) {
  if ($('settings').hidden) return;
  const list = $('pilots');
  const rows = s.pilots.map((p) => {
    const on = s.mode === 'watch' && Math.abs(p.mhz * 1e6 - s.watch_hz) < 2e6;
    const name = p.name || channelName(p.mhz);
    return `<div class="row${on ? ' on' : ''}" data-mhz="${p.mhz}"><b>${name}</b>` +
      `<span>${Math.round(p.mhz)} MHz <span class="dim">· ${p.std}</span></span>` +
      `<span><span class="dim">SNR</span> ${Math.round(p.snr)} dB</span><span class="tag">${on ? 'Watching' : ''}</span></div>`;
  }).join('');
  if (list.dataset.html !== rows) {
    list.innerHTML = rows;
    list.dataset.html = rows;
  }
}
$('pilots').addEventListener('click', (e) => {
  const row = e.target.closest('.row');
  if (row) cmd(`watch ${row.dataset.mhz}`);
});
$('scan').onclick = () => startScan('settings');
$('rescan').onclick = () => startScan('card');
$('connect').onclick = () => connect(true);

// ---- dialogs ----
function openDialog(id) {
  $(id).hidden = false;
  if (id === 'settings' && last) renderPilots(last);
}
function closeDialogs() {
  $('settings').hidden = true;
  $('guide').hidden = true;
}
$('gear').onclick = () => openDialog('settings');
$('guide-link').onclick = (e) => {
  e.preventDefault();
  openDialog('guide');
};
for (const b of document.querySelectorAll('.backdrop')) {
  b.addEventListener('pointerdown', (e) => e.target === b && closeDialogs());
  b.querySelector('[data-close]').onclick = closeDialogs;
}

// ---- OSD pill: drag from anywhere but the ✕, hide / show ----
const osd = $('osd');
function showOsd(on) {
  osd.hidden = !on;
  $('osd-show').hidden = on || phase !== 'watching';
  if (on) placeOsd();
}
function placeOsd() {
  const p = store.get('osdPos', null); // fractions of the stage, so it survives resizes
  if (!p) return;
  const r = stage.getBoundingClientRect(), o = osd.getBoundingClientRect();
  osd.style.left = `${Math.max(0, Math.min(r.width - o.width, p.x * r.width))}px`;
  osd.style.top = `${Math.max(0, Math.min(r.height - o.height, p.y * r.height))}px`;
}
window.addEventListener('resize', placeOsd);
$('osd-hide').onclick = () => {
  store.set('osdHidden', true);
  showOsd(false);
};
$('osd-show').onclick = () => {
  store.set('osdHidden', false);
  showOsd(true);
};
osd.addEventListener('pointerdown', (e) => {
  if (e.target.closest('button')) return;
  e.preventDefault();
  const r = stage.getBoundingClientRect(), o = osd.getBoundingClientRect();
  const dx = e.clientX - o.left, dy = e.clientY - o.top;
  osd.classList.add('dragging');
  osd.setPointerCapture(e.pointerId);
  const move = (ev) => {
    const x = Math.max(0, Math.min(r.width - o.width, ev.clientX - r.left - dx));
    const y = Math.max(0, Math.min(r.height - o.height, ev.clientY - r.top - dy));
    osd.style.left = `${x}px`;
    osd.style.top = `${y}px`;
    store.set('osdPos', { x: x / r.width, y: y / r.height });
  };
  const up = () => {
    osd.classList.remove('dragging');
    osd.removeEventListener('pointermove', move);
    osd.removeEventListener('pointerup', up);
  };
  osd.addEventListener('pointermove', move);
  osd.addEventListener('pointerup', up);
});

// ---- full screen, keys, idle dimming ----
const toggleFullscreen = () => (document.fullscreenElement ? document.exitFullscreen() : stage.requestFullscreen());
$('fs').onclick = toggleFullscreen;
document.addEventListener('fullscreenchange', () => {
  $('fs-icon').setAttribute('d', document.fullscreenElement
    ? 'M5.5 1.5v4h-4M14.5 5.5h-4v-4M10.5 14.5v-4h4M1.5 10.5h4v4'
    : 'M1.5 5.5v-4h4M10.5 1.5h4v4M14.5 10.5v4h-4M5.5 14.5h-4v-4');
  $('fs').title = document.fullscreenElement ? 'Exit full screen (F)' : 'Full screen (F)';
});
document.addEventListener('keydown', (e) => {
  if (e.ctrlKey || e.metaKey || e.altKey || e.target.closest?.('input, textarea')) return;
  if (e.key === 'Escape') return closeDialogs();
  const dialogOpen = !$('settings').hidden || !$('guide').hidden;
  if ((e.key === 'f' || e.key === 'F') && !dialogOpen) toggleFullscreen();
  if ((e.key === 'o' || e.key === 'O') && phase === 'watching' && !dialogOpen) {
    store.set('osdHidden', !osd.hidden);
    showOsd(osd.hidden);
  }
});
let idleTimer;
function wake() {
  stage.classList.remove('idle');
  clearTimeout(idleTimer);
  idleTimer = setTimeout(() => stage.classList.add('idle'), 2500);
}
stage.addEventListener('pointermove', wake);
wake();

// ---- unplug ----
navigator.usb?.addEventListener('disconnect', (e) => {
  if (switching || phase === 'start' || !ADAPTERS.some((a) => a.productId === e.device.productId)) return;
  cmd('close');
  fail('The adapter was unplugged. Plug it back in and click Connect.');
});

// ---- ?debug: stream tests, live numbers, the log ----
const logLines = [];
function log(text) {
  if (!DEBUG) return;
  logLines.push(`${new Date().toLocaleTimeString()} ${text}`);
  if (logLines.length > 300) logLines.shift();
}
let dbgPrev = null;
function renderDebug() {
  if (!DEBUG || !last) return;
  const s = last, v = s.video || {};
  let msps = 0;
  if (dbgPrev) msps = ((s.banks - dbgPrev.banks) * 8192) / ((s.t - dbgPrev.t) / 1000) / 1e6;
  dbgPrev = s;
  $('dbg-stats').textContent =
    `build ${BUILD}  ${navigator.userAgent.match(/(Chrome|Edg)\/[\d.]+/)?.[0] ?? ''}  isolated ${crossOriginIsolated}\n` +
    `open ${s.open}  usb3 ${s.usb3}  chip ${s.chip}  rx ${s.rx_state} ${s.rx_error}\n` +
    `mode ${s.mode}  fs ${(s.fs / 1e6).toFixed(0)} MS/s  watch ${s.watch_msps} MS/s${s.fallback ? ' (fallback)' : ''}\n` +
    `received ${msps.toFixed(2)} MS/s  banks ${s.banks}  dropped ${s.dropped}  decode_dropped ${s.decode_dropped}  restarts ${s.restarts}\n` +
    `last fault ${s.last_fault || '-'}\n` +
    `dsp ${(+s.dsp_load).toFixed(2)}  decode ${(+s.decode_load).toFixed(2)}  snr ${s.snr_db?.toFixed(1) ?? '-'} dB\n` +
    `video lock ${(100 * (v.lock || 0)).toFixed(1)} % ${v.std} colour ${v.colour} acq ${v.acquisitions} losses ${v.losses} ${(v.fps || 0).toFixed(0)} fps\n` +
    `frames sent ${s.sent} skipped ${s.skipped} painted ${painted}  decode→paint ${latencyMs?.toFixed(1) ?? '-'} ms\n` +
    `test ${s.test ? JSON.stringify(s.test) : '-'}`;
  $('dbg-log').textContent = logLines.slice(-80).reverse().join('\n');
}
if (DEBUG) {
  const d = $('debug');
  d.hidden = false;
  d.innerHTML = '<div><button data-c="test 2 20">Test 20 MS/s 20 s</button><button data-c="test 1 20">Test 40 MS/s 20 s</button>' +
    '<button data-c="idle">Idle</button><button id="dbg-copy">Copy report</button></div>' +
    '<pre id="dbg-stats">waiting for the receiver…</pre><pre class="log" id="dbg-log"></pre>';
  d.addEventListener('click', (e) => e.target.dataset.c && cmd(e.target.dataset.c));
  $('dbg-copy').onclick = () => navigator.clipboard?.writeText(`${$('dbg-stats').textContent}\n\n${logLines.join('\n')}`);
}

// ---- start ----
if (!navigator.usb) {
  card('kepyur needs Chrome or Edge on a computer: this browser cannot reach USB devices (WebUSB).', { help: false });
  $('connect').hidden = true;
} else {
  card('Analog FPV video in your browser.', { connect: true });
  // one paired adapter already plugged in: connect without a click
  pairedAdapters().then((ds) => ds.length === 1 && connect(false));
}
