// Hosts kepyur.wasm (web/kepyur_web.cpp). Its main thread serves libusb's proxied WebUSB calls and the stream's
// bulk-IN reads; it never blocks.
// Page -> worker: {t:'cmd', text}, {t:'ack'} (a frame was painted).
// Worker -> page: {t:'ready'}, {t:'fatal', msg}, {t:'ev', json}, {t:'frame', buf, w, h, ts}, {t:'log', text}.
import createKepyur from './kepyur.js';

const queue = [];
let mod = null;
const handle = (m) => {
  if (m.t === 'cmd') {
    const p = mod.stringToNewUTF8(m.text);
    mod._kp_cmd(p);
    mod._free(p);
  } else if (m.t === 'ack') mod._kp_frame_ack();
};
self.onmessage = ({ data }) => (mod ? handle(data) : queue.push(data));

// The stream's bulk-IN reader (wifi-sdr src/bulkin_js.h has the contract): `depth` transferIn() on EP 4 always in
// flight; each completion goes to the next ring slot in the wasm heap, flagged with an atomic the stream thread
// waits on, and the next transferIn is submitted at once.
const ADAPTERS = [[0x2357, 0x0115], [0x2357, 0x012d], [0x2357, 0x0138], [0x0bda, 0xb812]];
const bulk = { gen: 0, inflight: 0, M: null };
const openedAdapter = async () =>
  (await navigator.usb.getDevices()).find((d) => d.opened && ADAPTERS.some(([v, p]) => d.vendorId === v && d.productId === p));
function wsdrBulkStart(ctl, done, len, status, buf, ring, depth, slot) {
  const gen = ++bulk.gen;
  const H32 = bulk.M.HEAP32, U8 = bulk.M.HEAPU8;
  let n = 0;
  const put = (bytes, st) => {
    const k = n++ % ring;
    if (bytes) U8.set(bytes, buf + k * slot);
    H32[(len >> 2) + k] = bytes ? bytes.length : 0;
    H32[(status >> 2) + k] = st;
    Atomics.store(H32, (done >> 2) + k, 1);
    Atomics.notify(H32, (done >> 2) + k);
  };
  (async () => {
    const dev = await openedAdapter();
    if (gen !== bulk.gen) return;
    if (!dev) return put(null, 1);
    const submit = () => {
      bulk.inflight++;
      dev.transferIn(4, slot).then((r) => {
        bulk.inflight--;
        if (gen !== bulk.gen) return;
        put(new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength), r.status === 'ok' ? 0 : 1);
        submit();
      }, (e) => {
        bulk.inflight--;
        if (gen !== bulk.gen) return;
        self.postMessage({ t: 'log', text: `bulk-IN: ${e}` });
        put(null, 1);
      });
    };
    for (let i = 0; i < depth; i++) submit();
  })();
}
function wsdrBulkStop(ctl) {
  const gen = ++bulk.gen; // completions of the old run are ignored from here on
  const H32 = bulk.M.HEAP32;
  (async () => {
    const dev = await openedAdapter();
    if (dev && bulk.inflight > 0) {
      try { // WebUSB cannot cancel a transfer: releasing the interface aborts every pending one
        await dev.releaseInterface(0);
        await dev.claimInterface(0);
      } catch (e) {
        self.postMessage({ t: 'log', text: `bulk-IN abort: ${e}` });
      }
    }
    const t0 = performance.now();
    while (bulk.inflight > 0 && performance.now() - t0 < 800) await new Promise((r) => setTimeout(r, 1));
    if (gen !== bulk.gen) return;
    Atomics.store(H32, ctl >> 2, 1);
    Atomics.notify(H32, ctl >> 2);
  })();
}

try {
  const m = await createKepyur({
    wsdrBulkStart,
    wsdrBulkStop,
    onEvent: (json) => self.postMessage({ t: 'ev', json }),
    onFrame: (u8, w, h, ts) => self.postMessage({ t: 'frame', buf: u8.buffer, w, h, ts }, [u8.buffer]),
    print: (text) => self.postMessage({ t: 'log', text }),
    printErr: (text) => self.postMessage({ t: 'log', text }),
  });
  bulk.M = m;
  m._kp_init();
  mod = m;
  self.postMessage({ t: 'ready' });
  for (const q of queue.splice(0)) handle(q);
} catch (e) {
  self.postMessage({ t: 'fatal', msg: String(e?.message ?? e) });
}
