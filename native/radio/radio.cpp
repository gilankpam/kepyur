#include "radio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "log.h"
#include "receiver.h"
#include "spectrum.h"
#include "stream88xx.h"

#include <sys/resource.h>
#include <unistd.h>

namespace kp {

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

namespace {
constexpr double kFs = 40e6;
constexpr double kEdge = 16.5e6;     // usable +- span at 40 MS/s (BW40 DFIR is flat to ~17 MHz)
constexpr double kStep = 20e6;       // a hump up to ~13 MHz wide is fully inside some step's span
constexpr double kCapture = 0.09;    // s of IQ per scan step (60 ms acquisition + ~300 lines)
constexpr double kPsdSpan = 0.012;   // s of it for the spectrum
constexpr int kSettleBanks = 40;     // ~8 ms after a retune (chip AGC)
// A watch at 40 MS/s that restarts this often falls back to 20 MS/s (same picture: the BW20 RX filter is
// +-10 MHz either way). A link that cannot carry 40 MS/s restarts every 0.2-0.3 s (Chrome's WebUSB: ~14k
// transfers/s against the 24.4k needed); a healthy one runs minutes without one.
constexpr int kFallbackFaults = 3;
constexpr int64_t kFallbackWindowMs = 5000;

// Raise this thread's priority (the USB stream thread must never miss a chip deadline; the DSP thread
// must keep up). Best effort: logs what the OS allowed.
void boost_thread(const char *what) {
  const int tid = static_cast<int>(gettid());
  const int rc = setpriority(PRIO_PROCESS, tid, -10);
  KP_LOGI("radio: %s thread %d nice -10: %s", what, tid, rc == 0 ? "ok" : "refused");
}

std::vector<double> scan_plan() {
  std::vector<double> p;
  // the common analog bands first (A, B, E, F, R: 5645..5945), then the low band (L: 5362..5621)
  for (double c = 5652e6; c <= 5940e6; c += kStep) p.push_back(c);
  for (double c = 5370e6; c <= 5630e6; c += kStep) p.push_back(c);
  return p;
}
}  // namespace

Radio::Radio(FrameSink frames) : _frames(std::move(frames)) {}

Radio::~Radio() { close(); }

bool Radio::is_open() const {
  std::lock_guard<std::mutex> l(_m);
  return _open;
}

bool Radio::superspeed() const {
  std::lock_guard<std::mutex> l(_m);
  return _superspeed;
}

void Radio::open(int fd) {
  close();
  auto log = std::make_shared<Logger>();
  auto rx = std::make_unique<wsdr::Receiver>(log);
  wsdr::RxSettings s;
  s.freq = 5769e6;
  s.rate_sel = 1;  // 40 MS/s
  s.wide = true;
  s.agc = true;
  KP_LOGI("radio: opening adapter (%s)", fd >= 0 ? "Android fd" : "bus scan");
  rx->open(wsdr::kPid8812bu, s, -1, {}, fd);
  const auto caps = rx->caps();
  KP_LOGI("radio: %s, USB %s, fastest continuous rate %.1f MS/s", caps.chip.c_str(), caps.superspeed ? "3" : "2",
          wsdr::Receiver::rate_of(caps.min_rate_sel) / 1e6);
  if (!caps.superspeed) {
    // 40 MS/s does not stream on USB 2; open at a rate it can, so the stream still comes up for diagnostics
    s.rate_sel = caps.min_rate_sel;
    rx->set(s);
  }
  {
    std::lock_guard<std::mutex> l(_m);
    _superspeed = caps.superspeed;
    _chip = caps.chip;
    _open = true;
    _mode = Mode::Idle;
    _mode_epoch++;
    _req_hz = s.freq;
    _req_rate = s.rate_sel;
    _fallback = false;
  }
  _banks = 0;
  _dropped = 0;
  _stop = false;
  _rx = std::move(rx);
  _ddropped = 0;
  _dsp = std::thread([this] { dsp_loop(); });
  _decoder = std::thread([this] { decode_loop(); });
  _rx->start([this](const uint8_t *b, size_t n, const wsdr::BankMeta &m) {
    on_bank(b, n, m.seq, m.gen, m.fs, m.centre_hz, m.shift_hz);
  });
}

void Radio::close() {
  _stop = true;
  _qcv.notify_all();
  if (_rx) _rx->stop();
  if (_dsp.joinable()) _dsp.join();
  _dqcv.notify_all();
  if (_decoder.joinable()) _decoder.join();
  {
    std::lock_guard<std::mutex> l(_dqm);
    _dq.clear();
  }
  _fe.reset();
  if (_rx) {
    _rx->close();
    _rx.reset();
    KP_LOGI("radio: closed");
  }
  std::lock_guard<std::mutex> l(_m);
  _open = false;
  _mode = Mode::Idle;
  std::lock_guard<std::mutex> q(_qm);
  _q.clear();
}

// Receiver stream thread: copy only.
void Radio::on_bank(const uint8_t *b, size_t n, uint64_t seq, uint32_t gen, double fs, double centre, double shift) {
  thread_local bool boosted = false;
  if (!boosted) {
    boosted = true;
    boost_thread("usb stream");
  }
  std::vector<uint8_t> v;
  {
    std::lock_guard<std::mutex> l(_qm);
    if (!_pool.empty()) {
      v = std::move(_pool.back());
      _pool.pop_back();
    }
  }
  v.assign(b, b + n);
  {
    std::lock_guard<std::mutex> l(_qm);
    if (_q.size() >= _qcap) {
      _pool.push_back(std::move(_q.front().data));
      _q.pop_front();
      _dropped++;
    }
    _q.push_back(Bank{std::move(v), seq, gen, fs, centre, shift});
  }
  _banks++;
  _qcv.notify_one();
}

void Radio::tune(double hz, int rate_sel) {
  wsdr::RxSettings s = _rx->settings();
  s.freq = hz;
  s.rate_sel = rate_sel;
  s.wide = true;
  {
    std::lock_guard<std::mutex> l(_m);
    s.agc = _gain_db < 0;
    if (_gain_db >= 0) s.gain_db = _gain_db;
  }
  _rx->set(s);
  std::lock_guard<std::mutex> l(_m);
  _req_hz = hz;
  _req_rate = rate_sel;
}

void Radio::scan() {
  std::lock_guard<std::mutex> l(_m);
  _resume_hz = _mode == Mode::Watch ? _watch_hz : 0;
  _mode = Mode::Scan;
  _mode_epoch++;
}

void Radio::watch(double hz) {
  std::lock_guard<std::mutex> l(_m);
  _watch_hz = hz;
  _mode = Mode::Watch;
  _mode_epoch++;
}

void Radio::stream_test(int rate_sel, double seconds) {
  std::lock_guard<std::mutex> l(_m);
  _mode = Mode::Test;
  _mode_epoch++;
  _req_rate = rate_sel;
  _test_end_ms = static_cast<int64_t>(seconds * 1000);
}

void Radio::idle() {
  std::lock_guard<std::mutex> l(_m);
  _mode = Mode::Idle;
  _mode_epoch++;
}

void Radio::set_watch_rate(int rate_sel) {
  std::lock_guard<std::mutex> l(_m);
  _watch_rate = rate_sel;
  _fallback = false;
}

void Radio::set_gain(double db) {
  std::lock_guard<std::mutex> l(_m);
  _gain_db = db;
}

void Radio::record(double seconds, const std::string &prefix) {
  std::lock_guard<std::mutex> l(_m);
  _rec_req_s = seconds;
  _rec_req_prefix = prefix;
}

std::vector<Pilot> Radio::pilots() {
  std::lock_guard<std::mutex> l(_m);
  return _pilots;
}

void Radio::unpack(const Bank &b, std::vector<cf> &out) {
  const size_t ns = b.data.size() / 8;
  out.resize(ns);
  const uint8_t *p = b.data.data();
  for (size_t i = 0; i < ns; i++) {
    uint64_t w;
    std::memcpy(&w, p + 8 * i, 8);
    out[i] = cf(static_cast<float>(wsdr::la_i(w)), static_cast<float>(wsdr::la_q(w)));
  }
}

void Radio::dsp_loop() {
  boost_thread("dsp");
  Bank b;
  _load_t0_ms = now_ms();
  while (!_stop) {
    {
      std::unique_lock<std::mutex> l(_qm);
      _qcv.wait_for(l, std::chrono::milliseconds(200), [&] { return !_q.empty() || _stop; });
      if (_q.empty()) {
        if (_rx && !_rx->status().running) {
          KP_LOGE("radio: receiver stopped: %s", _rx->status().error.c_str());
          break;
        }
        continue;
      }
      b = std::move(_q.front());
      _q.pop_front();
    }
    const auto t0 = std::chrono::steady_clock::now();
    Mode mode;
    uint64_t epoch;
    {
      std::lock_guard<std::mutex> l(_m);
      mode = _mode;
      epoch = _mode_epoch;
    }
    const bool fresh = epoch != _epoch_seen;
    _epoch_seen = epoch;
    if (fresh) {
      // a mode change: set the radio up for it (a new watch builds a new decoder)
      _fe.reset();
      _fe_gen = ~0u;
      if (mode == Mode::Watch) {
        double hz;
        {
          std::lock_guard<std::mutex> l(_m);
          hz = _watch_hz;
        }
        // Watch at 40 MS/s with the chip's AGC, or 20 MS/s after a fallback (watch_fault). Both on a 40 MHz
        // channel: at 20 MHz width the OFDM CCA/AGC froze the LA in 120-sample runs (wifi-sdr RxSettings::wide).
        int rate;
        {
          std::lock_guard<std::mutex> l(_m);
          rate = _fallback && _watch_rate == 1 ? 2 : _watch_rate;
        }
        KP_LOGI("radio: watch %.3f MHz at %.0f MS/s", hz / 1e6, wsdr::Receiver::rate_of(rate) / 1e6);
        _watch_faults.clear();
        tune(hz, rate);
      } else if (mode == Mode::Scan) {
        _plan = scan_plan();
        _step = 0;
        _cap.clear();
        _settle = -1;
        KP_LOGI("radio: scan, %zu steps", _plan.size());
        tune(_plan[0], 1);
      } else if (mode == Mode::Test) {
        int rate;
        {
          std::lock_guard<std::mutex> l(_m);
          rate = _req_rate;
          _test_result.clear();
        }
        _test_banks = _test_gaps = 0;
        _test_last_seq = 0;
        _test_t0_ms = 0;
        _test_fs = wsdr::Receiver::rate_of(rate);
        KP_LOGI("radio: stream test at %.1f MS/s", _test_fs / 1e6);
        tune(5769e6, rate);
      }
    }
    switch (mode) {
    case Mode::Watch: handle_watch(b); break;
    case Mode::Scan: handle_scan(b); break;
    case Mode::Test: handle_test(b); break;
    case Mode::Idle: break;
    }
    {
      std::lock_guard<std::mutex> l(_qm);
      if (_pool.size() < _qcap) _pool.push_back(std::move(b.data));
    }
    _busy_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const int64_t t = now_ms();
    if (t - _load_t0_ms >= 1000) {
      std::lock_guard<std::mutex> l(_m);
      _load = _busy_s / ((t - _load_t0_ms) / 1000.0);
      _busy_s = 0;
      _load_t0_ms = t;
      if (++_status_ticks % 5 == 0 && _mode == Mode::Watch)
        KP_LOGI("status: watch %.1f MHz, cpu demod %.0f%% decode %.0f%%, dropped usb %llu decode %llu, restarts %llu, "
                "%s %s lock %.2f fps %.0f, acq %llu lost %llu rephase %llu", _watch_hz / 1e6, _load * 100, _dload * 100,
                (unsigned long long)_dropped.load(), (unsigned long long)_ddropped.load(),
                (unsigned long long)(_rx ? _rx->status().restarts : 0), _vstats.std_name,
                _vstats.colour ? "colour" : "grey", _vstats.lock_recent, _fps,
                (unsigned long long)_vstats.acquisitions, (unsigned long long)_vstats.losses,
                (unsigned long long)_vstats.rephases);
    }
  }
}

// A stream restart during a watch. At 40 MS/s, kFallbackFaults of them within kFallbackWindowMs switch the
// watch to 20 MS/s for the rest of the session; true when it did (the bank in hand is from the old stream).
bool Radio::watch_fault(double hz) {
  if (_fe_fs != 40e6) return false;
  const int64_t t = now_ms();
  _watch_faults.push_back(t);
  while (!_watch_faults.empty() && t - _watch_faults.front() > kFallbackWindowMs) _watch_faults.pop_front();
  if (static_cast<int>(_watch_faults.size()) < kFallbackFaults) return false;
  {
    std::lock_guard<std::mutex> l(_m);
    _fallback = true;
  }
  KP_LOGW("radio: 40 MS/s restarted %zu times in %lld s: watching at 20 MS/s from now on", _watch_faults.size(),
          static_cast<long long>(kFallbackWindowMs / 1000));
  _watch_faults.clear();
  _fe.reset();
  _fe_gen = ~0u;
  tune(hz, 2);
  return true;
}

void Radio::handle_watch(Bank &b) {
  double req;
  int req_rate;
  {
    std::lock_guard<std::mutex> l(_m);
    req = _req_hz;
    req_rate = _req_rate;
  }
  // ignore banks from before the retune (or the rate change of a fallback)
  if (std::fabs(b.centre_hz + b.shift_hz - req) > 1e3 || b.fs != wsdr::Receiver::rate_of(req_rate)) return;
  if (!_fe || b.fs != _fe_fs || b.shift_hz != _fe_shift) {
    _fe = std::make_shared<FrontEnd>(b.fs, b.shift_hz, _frames);
    _fe_fs = b.fs;
    _fe_shift = b.shift_hz;
  } else if (b.gen != _fe_gen) {
    // same settings: keep the decoder; it re-phases onto the restarted stream within a few lines
    std::string why;
    if (_rx) why = _rx->status().last_fault;
    KP_LOGW("radio: stream restarted (gen %u, %s), decoder keeps going", b.gen, why.c_str());
    if (watch_fault(req)) return;
  }
  _fe_gen = b.gen;
  unpack(b, _iq);
  record_bank(b);
  std::vector<float> d;
  {
    std::lock_guard<std::mutex> l(_dqm);
    if (!_dpool.empty()) {
      d = std::move(_dpool.back());
      _dpool.pop_back();
    }
  }
  _fe->demod(_iq.data(), _iq.size(), d);
  {
    std::lock_guard<std::mutex> l(_dqm);
    if (_dq.size() >= 128) {  // ~26 ms behind: drop the oldest (the decoder re-phases)
      _dpool.push_back(std::move(_dq.front().d));
      _dq.pop_front();
      _ddropped++;
    }
    _dq.push_back(DBlock{_fe, std::move(d)});
  }
  _dqcv.notify_one();
}

// Debug recording: the watched IQ as int16 (LA value << 4, as wsdr_probe writes .cs16), whole banks.
void Radio::record_bank(const Bank &b) {
  if (!_rec) {
    double secs;
    std::string prefix;
    {
      std::lock_guard<std::mutex> l(_m);
      secs = _rec_req_s;
      prefix = _rec_req_prefix;
      _rec_req_s = 0;
    }
    if (secs <= 0) return;
    _rec = std::fopen((prefix + ".cs16").c_str(), "wb");
    if (!_rec) {
      KP_LOGE("record: cannot open %s.cs16", prefix.c_str());
      return;
    }
    std::setvbuf(_rec, nullptr, _IOFBF, 1 << 20);
    _rec_prefix = prefix;
    _rec_left = static_cast<uint64_t>(secs * b.fs);
    _rec_samples = 0;
    _rec_fs = b.fs;
    _rec_centre = b.centre_hz;
    _rec_shift = b.shift_hz;
    KP_LOGI("record: %.1f s at %.0f MS/s -> %s.cs16", secs, b.fs / 1e6, prefix.c_str());
  }
  const size_t ns = std::min<uint64_t>(_iq.size(), _rec_left);
  std::vector<int16_t> buf(2 * ns);
  for (size_t i = 0; i < ns; i++) {
    buf[2 * i] = static_cast<int16_t>(static_cast<int>(_iq[i].real()) * 16);
    buf[2 * i + 1] = static_cast<int16_t>(static_cast<int>(_iq[i].imag()) * 16);
  }
  std::fwrite(buf.data(), 4, ns, _rec);
  _rec_left -= ns;
  _rec_samples += ns;
  if (_rec_left == 0) {
    std::fclose(_rec);
    _rec = nullptr;
    if (FILE *j = std::fopen((_rec_prefix + ".json").c_str(), "w")) {
      std::fprintf(j, "{\"format\":\"cs16\",\"sample_rate\":%.0f,\"centre_hz\":%.0f,\"shift_hz\":%.0f,\"samples\":%llu,"
                   "\"source\":\"kepyur watch\"}\n", _rec_fs, _rec_centre, _rec_shift, (unsigned long long)_rec_samples);
      std::fclose(j);
    }
    KP_LOGI("record: done, %llu samples -> %s.cs16", (unsigned long long)_rec_samples, _rec_prefix.c_str());
  }
}

void Radio::decode_loop() {
  boost_thread("decode");
  std::shared_ptr<FrontEnd> cur;
  double busy = 0;
  int64_t t0 = now_ms();
  while (!_stop) {
    DBlock blk;
    {
      std::unique_lock<std::mutex> l(_dqm);
      _dqcv.wait_for(l, std::chrono::milliseconds(200), [&] { return !_dq.empty() || _stop; });
      if (_dq.empty()) continue;
      blk = std::move(_dq.front());
      _dq.pop_front();
    }
    const auto ta = std::chrono::steady_clock::now();
    if (blk.fe != cur) {
      cur = blk.fe;
      _frames_at_last = 0;
      _fps_t0_ms = now_ms();
    }
    cur->decode(blk.d.data(), blk.d.size());
    busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - ta).count();
    {
      std::lock_guard<std::mutex> l(_dqm);
      if (_dpool.size() < 256) _dpool.push_back(std::move(blk.d));
    }
    const int64_t t = now_ms();
    if (t - _fps_t0_ms >= 1000) {
      const auto s = cur->stats();
      std::lock_guard<std::mutex> l(_m);
      _vstats = s;
      _fps = static_cast<float>((s.frames - _frames_at_last) * 1000.0 / (t - _fps_t0_ms));
      _frames_at_last = s.frames;
      _fps_t0_ms = t;
    }
    if (t - t0 >= 1000) {
      std::lock_guard<std::mutex> l(_m);
      _dload = busy / ((t - t0) / 1000.0);
      busy = 0;
      t0 = t;
    }
  }
}

void Radio::handle_scan(Bank &b) {
  if (_step >= _plan.size()) return;
  const double req = _plan[_step];
  if (std::fabs(b.centre_hz + b.shift_hz - req) > 1e3 || b.fs != kFs) return;
  if (_settle < 0) {
    _settle = kSettleBanks;
    _cap.clear();
    _cap_need = static_cast<size_t>(kPsdSpan * kFs);  // first just enough for the spectrum
    _humps.clear();
  }
  if (_settle > 0) {
    _settle--;
    _cap_last_seq = b.seq;
    return;
  }
  if (!_cap.empty() && b.seq != _cap_last_seq + 1) _cap.clear();  // a gap: start the capture over
  _cap_last_seq = b.seq;
  _cap_centre = b.centre_hz;
  const size_t ns = b.data.size() / 8;
  const uint8_t *p = b.data.data();
  for (size_t i = 0; i < ns && _cap.size() < _cap_need; i++) {
    uint64_t w;
    std::memcpy(&w, p + 8 * i, 8);
    _cap.emplace_back(static_cast<int16_t>(wsdr::la_i(w)), static_cast<int16_t>(wsdr::la_q(w)));
  }
  if (_cap.size() < _cap_need) return;
  if (_cap_need < static_cast<size_t>(kCapture * kFs)) {
    // spectrum first: a step with no candidate is done now; otherwise keep capturing for the sync check
    std::vector<cf> x(_cap.size());
    for (size_t i = 0; i < _cap.size(); i++) x[i] = cf(_cap[i].real(), _cap[i].imag());
    Psd psd(1024);
    psd.add(x.data(), x.size());
    _humps = find_humps(psd.db(), kFs, kEdge);
    if (!_humps.empty()) {
      _cap_need = static_cast<size_t>(kCapture * kFs);
      return;
    }
  }
  scan_step_done();
}

void Radio::scan_step_done() {
  const double centre = _cap_centre;  // the samples are centred on the hardware centre, not the request
  // start the next retune now; the chip settles while this step is analysed
  _step++;
  if (_step < _plan.size()) {
    tune(_plan[_step], 1);
    _settle = -1;
  }
  std::vector<cf> x(_humps.empty() ? 0 : _cap.size());
  for (size_t i = 0; i < x.size(); i++) x[i] = cf(_cap[i].real(), _cap[i].imag());
  const auto humps = _humps;
  std::vector<Pilot> found;
  for (const auto &h : humps) {
    const auto vc = check_video(x.data(), x.size(), kFs, h.offset_hz);
    const double mhz = (centre + h.offset_hz) / 1e6;
    KP_LOGI("scan: %.1f MHz hump %+.2f MHz (%.1f MHz wide, %.1f dB) -> %s %s lock %.2f", centre / 1e6,
            h.offset_hz / 1e6, h.width_hz / 1e6, h.snr_db, vc.video ? "VIDEO" : "no video", vc.std_name, vc.lock);
    if (!vc.video) continue;
    Pilot pl;
    pl.mhz = mhz;
    const auto *ch = nearest_channel(mhz);
    pl.name = ch ? ch->name : "";
    pl.std_name = vc.std_name;
    pl.snr_db = h.snr_db;
    pl.lock = vc.lock;
    pl.seen_ms = now_ms();
    found.push_back(pl);
  }
  bool done = _step >= _plan.size();
  double next_watch = 0;
  {
    std::lock_guard<std::mutex> l(_m);
    for (const auto &f : found) {
      auto it = std::find_if(_pilots.begin(), _pilots.end(), [&](const Pilot &p) { return std::fabs(p.mhz - f.mhz) < 4; });
      if (it == _pilots.end()) _pilots.push_back(f);
      else if (f.snr_db >= it->snr_db - 3) *it = f;  // prefer the newer sighting unless much weaker
      else it->seen_ms = f.seen_ms;
    }
    std::sort(_pilots.begin(), _pilots.end(), [](const Pilot &a, const Pilot &b) { return a.snr_db > b.snr_db; });
    if (done) {
      // after a scan: the pilot watched before, if still there, else the strongest seen in this scan
      const int64_t scan_start = now_ms() - 60000;
      for (const auto &p : _pilots)
        if (_resume_hz > 0 && std::fabs(p.mhz * 1e6 - _resume_hz) < 4e6) next_watch = p.mhz * 1e6;
      if (next_watch == 0)
        for (const auto &p : _pilots)
          if (p.seen_ms >= scan_start) {
            next_watch = p.mhz * 1e6;
            break;
          }
      if (next_watch == 0 && _resume_hz > 0) next_watch = _resume_hz;
      KP_LOGI("scan: done, %zu pilot(s)%s", _pilots.size(), next_watch > 0 ? ", watching" : "");
      if (next_watch > 0) {
        _watch_hz = next_watch;
        _mode = Mode::Watch;
      } else {
        _mode = Mode::Idle;
      }
      _mode_epoch++;
    }
  }
  _cap.clear();
}

void Radio::handle_test(Bank &b) {
  if (std::fabs(b.fs - _test_fs) > 1) return;
  const int64_t t = now_ms();
  if (_test_t0_ms == 0) {
    _test_t0_ms = t;
    _test_last_seq = b.seq;
    _test_gen = b.gen;
    _test_runs = 1;
    KP_LOGI("test: first bank at %.1f MS/s (gen %u)", b.fs / 1e6, b.gen);
  } else if (b.gen != _test_gen) {
    _test_gen = b.gen;  // a stream restart (seq starts over): counted as a run, not as a gap
    _test_runs++;
    KP_LOGW("test: stream restarted (gen %u): %s", b.gen, _rx->status().last_fault.c_str());
  } else if (b.seq != _test_last_seq + 1) {
    _test_gaps++;
  }
  _test_last_seq = b.seq;
  _test_banks++;
  const double el = (t - _test_t0_ms) / 1000.0;
  const double expect = el * b.fs / wsdr::Stream8822b::kBankSamples;
  std::ostringstream o;
  o << "{\"rate_msps\":" << b.fs / 1e6 << ",\"seconds\":" << el << ",\"banks\":" << _test_banks
    << ",\"expected\":" << static_cast<uint64_t>(expect) << ",\"seq_gaps\":" << _test_gaps
    << ",\"stream_runs\":" << _test_runs
    << ",\"restarts\":" << _rx->status().restarts << ",\"queue_dropped\":" << _dropped.load()
    << ",\"done\":" << (el * 1000 >= _test_end_ms ? "true" : "false") << "}";
  std::lock_guard<std::mutex> l(_m);
  _test_result = o.str();
  if (el * 1000 >= _test_end_ms) {
    KP_LOGI("test: result %s", _test_result.c_str());
    _mode = Mode::Idle;
    _mode_epoch++;
  }
}

static std::string jstr(const std::string &s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (static_cast<unsigned char>(c) < 0x20) continue;
    o += c;
  }
  return o + "\"";
}

std::string Radio::status_json() {
  std::ostringstream o;
  wsdr::RxStatus rs;
  if (_rx) rs = _rx->status();
  std::lock_guard<std::mutex> l(_m);
  static const char *modes[] = {"idle", "scan", "watch", "test"};
  o << "{\"open\":" << (_open ? "true" : "false") << ",\"usb3\":" << (_superspeed ? "true" : "false")
    << ",\"chip\":" << jstr(_chip) << ",\"rx_state\":" << jstr(wsdr::rx_state_name(rs.state))
    << ",\"rx_error\":" << jstr(rs.error) << ",\"last_fault\":" << jstr(rs.last_fault)
    << ",\"mode\":\"" << modes[static_cast<int>(_mode)] << "\",\"freq_hz\":" << _req_hz
    << ",\"watch_hz\":" << _watch_hz << ",\"fs\":" << rs.fs << ",\"banks\":" << _banks.load()
    << ",\"dropped\":" << _dropped.load() << ",\"decode_dropped\":" << _ddropped.load()
    << ",\"restarts\":" << rs.restarts << ",\"dsp_load\":" << _load << ",\"decode_load\":" << _dload
    << ",\"watch_msps\":" << (_mode == Mode::Watch ? wsdr::Receiver::rate_of(_req_rate) / 1e6 : 0)
    << ",\"fallback\":" << (_fallback ? "true" : "false");
  if (_mode == Mode::Scan) o << ",\"scan_step\":" << _step << ",\"scan_steps\":" << _plan.size();
  o << ",\"video\":{\"locked\":" << (!_vstats.acquiring && _vstats.lock_recent > 0.5f ? "true" : "false")
    << ",\"lock\":" << _vstats.lock_recent << ",\"std\":" << jstr(_vstats.std_name) << ",\"fps\":" << _fps
    << ",\"acquisitions\":" << _vstats.acquisitions << ",\"losses\":" << _vstats.losses
    << ",\"rephases\":" << _vstats.rephases << ",\"colour\":" << (_vstats.colour ? "true" : "false") << "}";
  o << ",\"test\":" << (_test_result.empty() ? "null" : _test_result);
  o << ",\"pilots\":[";
  const int64_t t = now_ms();
  for (size_t i = 0; i < _pilots.size(); i++) {
    const auto &p = _pilots[i];
    o << (i ? "," : "") << "{\"mhz\":" << p.mhz << ",\"name\":" << jstr(p.name) << ",\"std\":" << jstr(p.std_name)
      << ",\"snr\":" << p.snr_db << ",\"age_s\":" << (t - p.seen_ms) / 1000 << "}";
  }
  o << "]}";
  return o.str();
}

}  // namespace kp
