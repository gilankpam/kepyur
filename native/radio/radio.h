// kepyur radio: one RTL8812BU (wifi-sdr Receiver) + a DSP thread. Modes:
//   scan   - sweep the analog FPV bands at 40 MS/s, list pilots (humps that pass a sync check)
//   watch  - tune to one pilot and decode its video (frames to the FrameSink)
//   test   - stream at a given rate and only count banks (USB/controller soak, no DSP)
// The Receiver's sink only copies banks into a queue (wifi-sdr rule); everything else runs on the DSP thread.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "spectrum.h"
#include "video.h"

namespace wsdr {
class Receiver;
}

namespace kp {

struct Pilot {
  double mhz = 0;        // measured carrier
  std::string name;      // nearest standard channel ("R4"), "" if none
  std::string std_name;  // NTSC / PAL
  float snr_db = 0;
  float lock = 0;
  int64_t seen_ms = 0;   // steady-clock ms of the last scan that saw it
};

class Radio {
public:
  explicit Radio(FrameSink frames);
  ~Radio();
  Radio(const Radio &) = delete;
  Radio &operator=(const Radio &) = delete;

  // fd >= 0: the adapter behind this usbfs fd (Android); fd < 0: the first 8812BU on the bus (desktop).
  // Blocking (~5 s chip bring-up). Throws std::exception with a user-presentable message.
  void open(int fd);
  void close();
  bool is_open() const;
  bool superspeed() const;

  void scan();                 // then watch the previous pilot again, or the strongest found
  void watch(double hz);
  void stream_test(int rate_sel, double seconds);
  void idle();
  // Debug: the rate watch uses (1 = 40 MS/s, the default; 2 = 20 MS/s); takes effect on the next watch().
  // Also clears a 20 MS/s fallback.
  void set_watch_rate(int rate_sel);
  // Debug: gain for the next tune: < 0 = the chip's AGC, else fixed gain in dB (CCA off, IGI pinned).
  void set_gain(double db);
  // Debug: save the next `seconds` of watched IQ as PREFIX.cs16 + PREFIX.json (kp_replay reads them).
  void record(double seconds, const std::string &prefix);

  std::string status_json();   // for the UI (polled)
  std::vector<Pilot> pilots();

private:
  enum class Mode { Idle, Scan, Watch, Test };
  struct Bank {
    std::vector<uint8_t> data;
    uint64_t seq;
    uint32_t gen;
    double fs, centre_hz, shift_hz;
  };
  void dsp_loop();
  void decode_loop();
  void on_bank(const uint8_t *b, size_t n, uint64_t seq, uint32_t gen, double fs, double centre, double shift);
  void tune(double hz, int rate_sel);
  void handle_watch(Bank &b);
  bool watch_fault(double hz);
  void record_bank(const Bank &b);
  void handle_scan(Bank &b);
  void handle_test(Bank &b);
  void scan_step_done();
  void unpack(const Bank &b, std::vector<cf> &out);

  FrameSink _frames;
  std::unique_ptr<wsdr::Receiver> _rx;
  mutable std::mutex _m;  // mode, pilots, status fields below
  Mode _mode = Mode::Idle;
  uint64_t _mode_epoch = 0;  // bumped on every mode change; the DSP thread restarts its state
  double _watch_hz = 0, _req_hz = 0;
  int _req_rate = 1;
  int _watch_rate = 1;
  bool _fallback = false;  // 40 MS/s kept faulting during a watch: watch at 20 MS/s until open() / set_watch_rate()
  double _gain_db = -1;
  // recording (DSP thread writes; the request is under _m)
  double _rec_req_s = 0;
  std::string _rec_req_prefix;
  FILE *_rec = nullptr;
  std::string _rec_prefix;
  uint64_t _rec_left = 0, _rec_samples = 0;
  double _rec_fs = 0, _rec_centre = 0, _rec_shift = 0;
  std::vector<Pilot> _pilots;
  bool _superspeed = false, _open = false;
  std::string _chip;

  // bank queue
  std::mutex _qm;
  std::condition_variable _qcv;
  std::deque<Bank> _q;
  std::vector<std::vector<uint8_t>> _pool;
  size_t _qcap = 512;  // 100 ms at 40 MS/s
  std::atomic<uint64_t> _banks{0}, _dropped{0};

  std::thread _dsp;
  std::atomic<bool> _stop{false};

  // decode thread: watch mode runs demod on the DSP thread and the video decoder here (two cores)
  struct DBlock {
    std::shared_ptr<FrontEnd> fe;
    std::vector<float> d;
  };
  std::thread _decoder;
  std::mutex _dqm;
  std::condition_variable _dqcv;
  std::deque<DBlock> _dq;
  std::vector<std::vector<float>> _dpool;
  std::atomic<uint64_t> _ddropped{0};
  double _dload = 0;

  // DSP-thread state
  uint64_t _epoch_seen = ~0ull;
  std::shared_ptr<FrontEnd> _fe;
  uint32_t _fe_gen = ~0u;
  std::deque<int64_t> _watch_faults;  // ms of the stream restarts seen in this watch at 40 MS/s
  double _fe_fs = 0, _fe_shift = 0;
  std::vector<cf> _iq;
  // scan
  std::vector<double> _plan;
  size_t _step = 0;
  int _settle = 0;
  uint64_t _cap_last_seq = 0;
  std::vector<std::complex<int16_t>> _cap;
  size_t _cap_need = 0;
  double _cap_centre = 0;
  std::vector<Hump> _humps;  // this step's candidates (from the first kPsdSpan of the capture)  // hardware centre of the capture (humps are measured from it)
  double _resume_hz = 0;
  // test
  uint64_t _test_banks = 0, _test_gaps = 0, _test_last_seq = 0, _test_runs = 0;
  uint32_t _test_gen = 0;
  int64_t _test_t0_ms = 0, _test_end_ms = 0;
  double _test_fs = 0;
  std::string _test_result;
  // load
  double _busy_s = 0, _load = 0;
  int64_t _load_t0_ms = 0;
  uint64_t _status_ticks = 0;
  // video stats for status
  DecoderStats _vstats;
  uint64_t _frames_at_last = 0;
  int64_t _fps_t0_ms = 0;
  float _fps = 0;
};

int64_t now_ms();

}  // namespace kp
