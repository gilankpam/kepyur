// kepyur: power spectrum and analog-video signal finding (the scanner's DSP half).
#pragma once
#include <complex>
#include <cstddef>
#include <vector>

#include "video.h"

namespace kp {

// Averaged power spectrum, fftshifted (bin 0 = -fs/2), in dB. Hann window, 50 % overlap.
class Psd {
public:
  explicit Psd(int n = 1024);
  void reset();
  void add(const cf *x, size_t count);  // any count; leftovers carry to the next call
  int frames() const { return _frames; }
  int size() const { return _n; }
  std::vector<float> db() const;

private:
  int _n;
  std::vector<float> _win;
  std::vector<cf> _buf, _tmp;
  std::vector<double> _acc;
  size_t _fill = 0;
  int _frames = 0;
};

void fft_inplace(cf *x, int n);  // radix-2, n a power of two, forward

// A hump in the spectrum that looks like an FM video carrier (3.5..24 MHz wide at floor + 6 dB).
struct Hump {
  double offset_hz;  // centroid, relative to the stream centre
  double width_hz;
  float peak_db, snr_db;
};
// Humps fully inside +-edge_hz of the centre.
std::vector<Hump> find_humps(const std::vector<float> &db, double fs, double edge_hz);

// Live SNR of the carrier being watched (dB): the strongest point of the smoothed spectrum against the noise
// floor (10th percentile) inside +-span_hz, the band the RX filter passes. Comparable with Hump::snr_db for a
// signal that leaves part of the span free; a very wide, close signal reads lower.
float carrier_snr(const std::vector<float> &db, double fs, double span_hz);

// Sync check: is there analog video at this offset? Runs a FrontEnd over the samples.
struct VideoCheck {
  bool video = false;
  const char *std_name = "?";
  float lock = 0;  // locked fraction of lines at the end
  float dev_mhz = 0;  // sync tip to white estimate
};
VideoCheck check_video(const cf *x, size_t n, double fs, double offset_hz);

// Standard analog 5.8 GHz FPV channel names.
struct FpvChannel {
  const char *name;
  double mhz;
};
const std::vector<FpvChannel> &fpv_channels();
// Nearest standard channel within max_off_mhz, or nullptr.
const FpvChannel *nearest_channel(double mhz, double max_off_mhz = 6.0);

}  // namespace kp
