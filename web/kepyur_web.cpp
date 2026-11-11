// kepyur web: the page's side of the radio. A dedicated worker (web/site/worker.js) hosts this module;
// kepyur's Radio (native/radio) runs on pthreads exactly as on Android and the desktop.
//
// Threads: Radio's own (USB stream, DSP, decode), a command thread and a 500 ms status tick are pthreads.
// libusb forwards their USB calls to the module's main thread (the worker's), which must never block: the
// exports below only queue work. Events and frames go to the worker with MAIN_THREAD_ASYNC_EM_ASM.
//
// Commands (one line of text each), every one answered by a "reply" event:
//   usb3                 switch an 8812BU at USB 2 to USB 3 (it re-enumerates); text = wifi-sdr's result name
//   open                 bring the adapter up (~3 s) and watch at 20 MS/s (WebUSB cannot carry 40)
//   close | idle
//   scan [MHZ]           scan, then watch MHZ if found (or anyway), else the pilot being watched / the strongest
//   watch MHZ
//   test RATE_SEL SECS   stream test (?debug)
#include <emscripten.h>
#include <emscripten/threading.h>
#include <libusb.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "radio.h"
#include "receiver.h"
#include "usbmode.h"

namespace {

std::unique_ptr<kp::Radio> g_radio;
std::mutex g_m;
std::condition_variable g_cv;
std::deque<std::string> g_cmds;
std::atomic<bool> g_frame_busy{false};
std::atomic<uint64_t> g_sent{0}, g_skipped{0};

void to_page(const std::string &json) {
  char *p = strdup(json.c_str());
  MAIN_THREAD_ASYNC_EM_ASM({ Module.onEvent(UTF8ToString($0)); _free($0); }, p);
}

std::string jstr(const std::string &s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (static_cast<unsigned char>(c) < 0x20) continue;
    o += c;
  }
  return o + "\"";
}

void reply(const std::string &cmd, bool ok, const std::string &text) {
  to_page("{\"ev\":\"reply\",\"cmd\":" + jstr(cmd) + ",\"ok\":" + (ok ? "true" : "false") + ",\"text\":" +
          jstr(text) + "}");
}

// Decoder thread. One frame in flight to the page (it acks after painting); the rest are skipped.
void on_frame(const kp::Frame &f) {
  if (g_frame_busy.exchange(true)) {
    g_skipped++;
    return;
  }
  const size_t px = static_cast<size_t>(f.width) * f.height;
  auto *p = static_cast<uint32_t *>(std::malloc(px * 4));
  const auto *s = reinterpret_cast<const uint32_t *>(f.data);
  for (size_t i = 0; i < px; i++) p[i] = s[i] | 0xff000000u;  // RGBX -> opaque RGBA
  g_sent++;
  // Date.now(): the same clock on every thread and on the page (performance.now() is per thread)
  MAIN_THREAD_ASYNC_EM_ASM({ Module.onFrame(HEAPU8.slice($0, $0 + $1 * $2 * 4), $1, $2, $3); _free($0); }, p,
                           f.width, f.height, emscripten_date_now());
}

std::string port_path(libusb_device *d) {
  uint8_t ports[8];
  const int n = libusb_get_port_numbers(d, ports, 8);
  std::string s;
  for (int i = 0; i < n; i++) s += (i ? "." : "") + std::to_string(ports[i]);
  return s;
}

// The 8812BU enumerates at USB 2 after a plug; 40 MS/s scans need USB 3. Find the paired one and switch it.
std::string usb3() {
  libusb_context *ctx = nullptr;
  if (libusb_init(&ctx) != 0) return "Failed: libusb_init";
  libusb_device **list = nullptr;
  const ssize_t n = libusb_get_device_list(ctx, &list);
  int bus = -1, speed = 0;
  std::string port;
  for (ssize_t i = 0; i < n; i++) {
    libusb_device_descriptor dd{};
    if (libusb_get_device_descriptor(list[i], &dd) == 0 &&
        wsdr::model_pid(dd.idVendor, dd.idProduct) == wsdr::kPid8812bu) {
      bus = libusb_get_bus_number(list[i]);
      port = port_path(list[i]);
      speed = libusb_get_device_speed(list[i]);
      break;
    }
  }
  if (list) libusb_free_device_list(list, 1);
  libusb_exit(ctx);
  if (bus < 0) return "NotFound";
  if (speed >= LIBUSB_SPEED_SUPER) return "NotNeeded";
  std::string detail;
  const auto r = wsdr::request_usb3(wsdr::kPid8812bu, bus, port, &detail);
  return std::string(wsdr::usb3_switch_name(r)) + (detail.empty() ? "" : ": " + detail);
}

void run(const std::string &line) {
  std::istringstream in(line);
  std::string op;
  in >> op;
  try {
    if (op == "usb3") {
      reply(op, true, usb3());
    } else if (op == "open") {
      g_radio->open(-1);
      g_radio->set_watch_rate(2);
      reply(op, true, g_radio->superspeed() ? "USB 3" : "USB 2");
    } else if (op == "close") {
      g_radio->close();
      reply(op, true, "closed");
    } else if (op == "idle") {
      g_radio->idle();
      reply(op, true, "idle");
    } else if (op == "scan") {
      double mhz = 0;
      in >> mhz;
      g_radio->scan(mhz * 1e6);
      reply(op, true, "scanning");
    } else if (op == "watch") {
      double mhz = 0;
      in >> mhz;
      g_radio->watch(mhz * 1e6);
      reply(op, true, "watching");
    } else if (op == "test") {
      int r = 2;
      double secs = 20;
      in >> r >> secs;
      g_radio->stream_test(r, secs);
      reply(op, true, "stream test started");
    } else {
      reply(op, false, "unknown command");
    }
  } catch (const std::exception &e) {
    reply(op, false, e.what());
  }
}

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE void kp_init() {
  if (g_radio) return;
  g_radio = std::make_unique<kp::Radio>(on_frame);
  std::thread([] {
    for (;;) {
      std::unique_lock<std::mutex> l(g_m);
      g_cv.wait(l, [] { return !g_cmds.empty(); });
      const std::string c = std::move(g_cmds.front());
      g_cmds.pop_front();
      l.unlock();
      run(c);
    }
  }).detach();
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      to_page("{\"ev\":\"status\",\"t\":" + std::to_string(emscripten_date_now()) + ",\"sent\":" +
              std::to_string(g_sent.load()) + ",\"skipped\":" + std::to_string(g_skipped.load()) +
              ",\"st\":" + g_radio->status_json() + "}");
    }
  }).detach();
}

EMSCRIPTEN_KEEPALIVE void kp_cmd(const char *line) {
  {
    std::lock_guard<std::mutex> l(g_m);
    g_cmds.emplace_back(line);
  }
  g_cv.notify_one();
}

// The page painted the last frame.
EMSCRIPTEN_KEEPALIVE void kp_frame_ack() { g_frame_busy = false; }

}  // extern "C"
