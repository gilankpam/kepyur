// kp_live: kepyur's radio (scan / watch / stream test) on the desktop with the adapter plugged in.
//   kp_live --scan [--frames]               scan, then watch the strongest pilot
//   kp_live --watch 5769 [--frames]         watch one frequency (MHz)
//   kp_live --test 1 60                     stream test: rate_sel (1 = 40 MS/s, 2 = 20 MS/s), seconds
//   ... --frames | ffplay -f rawvideo -pixel_format rgb0 -video_size 720x480 -framerate 60 -fflags nobuffer -i -
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include <libusb.h>

#include "log.h"
#include "radio.h"
#include "receiver.h"
#include "usbmode.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <timeapi.h>
#endif

using namespace kp;
static std::atomic<bool> g_stop{false};

// A cold-plugged 8812BU enumerates at USB 2 (on Linux rtw88 switches it at probe; Windows has no such
// driver once WinUSB is bound). Ask it for USB 3 and wait for it to come back, as the Android app does.
static void try_usb3() {
  libusb_context *ctx = nullptr;
  if (libusb_init(&ctx) != 0) return;
  libusb_device **list = nullptr;
  int bus = -1;
  std::string port;
  for (ssize_t i = 0, n = libusb_get_device_list(ctx, &list); i < n; i++) {
    libusb_device_descriptor dd{};
    if (libusb_get_device_descriptor(list[i], &dd) != 0 || wsdr::model_pid(dd.idVendor, dd.idProduct) != wsdr::kPid8812bu)
      continue;
    uint8_t p[8];
    const int np = libusb_get_port_numbers(list[i], p, sizeof p);
    for (int j = 0; j < np; j++) port += (j ? "." : "") + std::to_string(p[j]);
    bus = libusb_get_bus_number(list[i]);
    break;
  }
  if (list) libusb_free_device_list(list, 1);
  libusb_exit(ctx);
  if (bus < 0) return;  // no 8812BU: let open() report it
  std::string detail;
  const auto r = wsdr::request_usb3(wsdr::kPid8812bu, bus, port, &detail);
  std::fprintf(stderr, "usb3 switch: %s%s%s\n", wsdr::usb3_switch_name(r), detail.empty() ? "" : ": ", detail.c_str());
  if (r == wsdr::Usb3Switch::Requested) std::this_thread::sleep_for(std::chrono::seconds(4));  // re-enumeration
}

int main(int argc, char **argv) {
  std::string mode = "scan";
  double mhz = 5769, secs = 60;
  int rate = 1;
  int watch_rate = 1;  // 40 MS/s (--watch-rate 2: 20 MS/s)
  bool frames = false, usb3 = true;
  for (int i = 1; i < argc; i++) {
    const std::string k = argv[i];
    if (k == "--scan") mode = "scan";
    else if (k == "--watch" && i + 1 < argc) { mode = "watch"; mhz = std::atof(argv[++i]); }
    else if (k == "--test" && i + 2 < argc) { mode = "test"; rate = std::atoi(argv[++i]); secs = std::atof(argv[++i]); }
    else if (k == "--frames") frames = true;
    else if (k == "--no-usb3") usb3 = false;
    else if (k == "--watch-rate" && i + 1 < argc) watch_rate = std::atoi(argv[++i]);
    else if (k == "-v") set_log_debug(true);
    else { std::fprintf(stderr, "usage: kp_live [--scan | --watch MHZ | --test RATE_SEL SECONDS] [--frames] [--watch-rate 1|2] [--no-usb3] [-v]\n"); return 2; }
  }
  std::signal(SIGINT, [](int) { g_stop = true; });
  std::signal(SIGTERM, [](int) { g_stop = true; });
#ifdef _WIN32
  timeBeginPeriod(1);  // 1 ms timer: keeps the CPUs out of their deepest idle states (USB interrupt latency)
  _setmode(_fileno(stdout), _O_BINARY);  // raw frames: no CRLF translation
#else
  std::signal(SIGPIPE, [](int) { g_stop = true; });
#endif
  Radio radio([&](const Frame &f) {
    if (frames && !g_stop) {
      const size_t n = static_cast<size_t>(f.width) * f.height * 4;
      if (std::fwrite(f.data, 1, n, stdout) != n || std::fflush(stdout) != 0) g_stop = true;  // player closed
    }
  });
  if (usb3 && !std::getenv("KP_SIM_FILE")) try_usb3();
  try {
    radio.open(-1);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "open failed: %s\n", e.what());
    return 1;
  }
  radio.set_watch_rate(watch_rate);
  if (mode == "scan") radio.scan();
  else if (mode == "watch") radio.watch(mhz * 1e6);
  else radio.stream_test(rate, secs);
  const auto t0 = std::chrono::steady_clock::now();
  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const std::string st = radio.status_json();
    std::fprintf(stderr, "%s\n", st.c_str());
    if (mode == "test" && st.find("\"done\":true") != std::string::npos) break;
    if (mode == "test" && std::chrono::steady_clock::now() - t0 > std::chrono::duration<double>(secs + 30)) break;
  }
  radio.close();
  return 0;
}
