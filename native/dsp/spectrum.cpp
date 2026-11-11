#include "spectrum.h"

#include <algorithm>
#include <cmath>

namespace kp {

void fft_inplace(cf *x, int n) {
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(x[i], x[j]);
  }
  for (int len = 2; len <= n; len <<= 1) {
    const double a = -2 * M_PI / len;
    const cf wl(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
    for (int i = 0; i < n; i += len) {
      cf w(1, 0);
      for (int k = 0; k < len / 2; k++) {
        const cf u = x[i + k], v = x[i + k + len / 2] * w;
        x[i + k] = u + v;
        x[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

Psd::Psd(int n) : _n(n), _win(n), _buf(n), _tmp(n), _acc(n, 0.0) {
  for (int i = 0; i < n; i++) _win[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * M_PI * i / n));
}

void Psd::reset() {
  std::fill(_acc.begin(), _acc.end(), 0.0);
  _fill = 0;
  _frames = 0;
}

void Psd::add(const cf *x, size_t count) {
  for (size_t i = 0; i < count; i++) {
    _buf[_fill++] = x[i];
    if (_fill < static_cast<size_t>(_n)) continue;
    for (int k = 0; k < _n; k++) _tmp[k] = _buf[k] * _win[k];
    fft_inplace(_tmp.data(), _n);
    for (int k = 0; k < _n; k++) _acc[k] += std::norm(_tmp[k]);
    _frames++;
    // 50 % overlap
    std::copy(_buf.begin() + _n / 2, _buf.end(), _buf.begin());
    _fill = _n / 2;
  }
}

std::vector<float> Psd::db() const {
  std::vector<float> out(_n);
  const double norm = _frames ? 1.0 / _frames : 1.0;
  for (int k = 0; k < _n; k++) {
    const int src = (k + _n / 2) % _n;  // fftshift
    out[k] = static_cast<float>(10 * std::log10(_acc[src] * norm + 1e-20));
  }
  return out;
}

namespace {
// Median-smoothed spectrum: kills the DC line and narrow carriers (~0.6 MHz window).
std::vector<float> smooth_db(const std::vector<float> &db, double bin) {
  const int n = static_cast<int>(db.size());
  const int hw = std::max(2, static_cast<int>(0.3e6 / bin));
  std::vector<float> sm(n), win;
  for (int i = 0; i < n; i++) {
    win.clear();
    for (int k = std::max(0, i - hw); k <= std::min(n - 1, i + hw); k++) win.push_back(db[k]);
    std::nth_element(win.begin(), win.begin() + win.size() / 2, win.end());
    sm[i] = win[win.size() / 2];
  }
  return sm;
}
}  // namespace

std::vector<Hump> find_humps(const std::vector<float> &db, double fs, double edge_hz) {
  const int n = static_cast<int>(db.size());
  const double bin = fs / n;
  // median-smooth, then floor = lower quartile of the smoothed spectrum
  const std::vector<float> sm = smooth_db(db, bin);
  std::vector<float> sorted = sm;
  std::nth_element(sorted.begin(), sorted.begin() + n / 4, sorted.end());
  const float floor = sorted[n / 4];  // lower quartile: robust even when a wide signal fills much of the span
  const float thr = floor + 6.f;
  std::vector<Hump> out;
  int i = 0;
  while (i < n) {
    if (sm[i] <= thr) {
      i++;
      continue;
    }
    int j = i;
    while (j < n && sm[j] > thr) j++;
    const double f0 = (i - n / 2) * bin, f1 = (j - 1 - n / 2) * bin;
    const double width = f1 - f0 + bin;
    if (width >= 3.5e6 && width <= 24e6 && f0 > -edge_hz && f1 < edge_hz) {  // a VTX next to the phone: ~16 MHz
      double sw = 0, sf = 0;
      float pk = -1e9f;
      for (int k = i; k < j; k++) {
        const double p = std::pow(10.0, (sm[k] - floor) / 10.0);
        sw += p;
        sf += p * (k - n / 2) * bin;
        pk = std::max(pk, sm[k]);
      }
      out.push_back(Hump{sf / sw, width, pk, pk - floor});
    }
    i = j;
  }
  return out;
}

float carrier_snr(const std::vector<float> &db, double fs, double span_hz) {
  const int n = static_cast<int>(db.size());
  const double bin = fs / n;
  const std::vector<float> sm = smooth_db(db, bin);
  std::vector<float> in;
  float pk = -1e9f;
  for (int i = 0; i < n; i++) {
    if (std::fabs((i - n / 2) * bin) > span_hz) continue;
    in.push_back(sm[i]);
    pk = std::max(pk, sm[i]);
  }
  if (in.empty()) return 0;
  std::nth_element(in.begin(), in.begin() + in.size() / 10, in.end());
  return pk - in[in.size() / 10];
}

VideoCheck check_video(const cf *x, size_t n, double fs, double offset_hz) {
  DecoderOpts o;
  FrontEnd fe(fs, offset_hz, nullptr, o);
  fe.feed(x, n);
  const DecoderStats s = fe.stats();
  VideoCheck r;
  r.video = s.acquisitions > 0 && s.lines > 200 && s.lock_recent > 0.6f;
  r.std_name = s.std_name;
  r.lock = s.lock_recent;
  r.dev_mhz = 3.5f * (s.blank - s.tip);
  return r;
}

const std::vector<FpvChannel> &fpv_channels() {
  static const std::vector<FpvChannel> ch = {
      {"A1", 5865}, {"A2", 5845}, {"A3", 5825}, {"A4", 5805}, {"A5", 5785}, {"A6", 5765}, {"A7", 5745}, {"A8", 5725},
      {"B1", 5733}, {"B2", 5752}, {"B3", 5771}, {"B4", 5790}, {"B5", 5809}, {"B6", 5828}, {"B7", 5847}, {"B8", 5866},
      {"E1", 5705}, {"E2", 5685}, {"E3", 5665}, {"E4", 5645}, {"E5", 5885}, {"E6", 5905}, {"E7", 5925}, {"E8", 5945},
      {"F1", 5740}, {"F2", 5760}, {"F3", 5780}, {"F4", 5800}, {"F5", 5820}, {"F6", 5840}, {"F7", 5860}, {"F8", 5880},
      {"R1", 5658}, {"R2", 5695}, {"R3", 5732}, {"R4", 5769}, {"R5", 5806}, {"R6", 5843}, {"R7", 5880}, {"R8", 5917},
      {"L1", 5362}, {"L2", 5399}, {"L3", 5436}, {"L4", 5473}, {"L5", 5510}, {"L6", 5547}, {"L7", 5584}, {"L8", 5621},
  };
  return ch;
}

const FpvChannel *nearest_channel(double mhz, double max_off_mhz) {
  const FpvChannel *best = nullptr;
  double bd = max_off_mhz;
  // Raceband first on ties: it is by far the most common
  static const char order[] = {'R', 'F', 'A', 'B', 'E', 'L'};
  for (char band : order)
    for (const auto &c : fpv_channels()) {
      if (c.name[0] != band) continue;
      const double d = std::fabs(c.mhz - mhz);
      if (d < bd - 0.01) {
        bd = d;
        best = &c;
      }
    }
  return best;
}

}  // namespace kp
