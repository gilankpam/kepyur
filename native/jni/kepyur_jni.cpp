// kepyur JNI: the Kotlin app's view of the radio (com.kepyur.app.Native).
// Frames go straight to the video Surface on a render thread (latest frame wins, the DSP thread never waits).
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "log.h"
#include "radio.h"
#include "usbmode.h"

using namespace kp;

namespace {

// ---- log file ----
std::mutex g_log_m;
FILE *g_log = nullptr;
void file_sink(LogLevel l, const char *line) {
  std::lock_guard<std::mutex> lk(g_log_m);
  if (!g_log) return;
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  tm t;
  localtime_r(&ts.tv_sec, &t);
  static const char tag[] = {'D', 'I', 'W', 'E'};
  std::fprintf(g_log, "%02d:%02d:%02d.%03ld %c %s\n", t.tm_hour, t.tm_min, t.tm_sec, ts.tv_nsec / 1000000,
               tag[static_cast<int>(l)], line);
  std::fflush(g_log);
}

// ---- video output ----
class Renderer {
public:
  Renderer() : _th([this] { loop(); }) {}
  ~Renderer() {
    {
      std::lock_guard<std::mutex> l(_m);
      _stop = true;
    }
    _cv.notify_all();
    _th.join();
    set_window(nullptr);
  }
  void set_window(ANativeWindow *w) {
    std::lock_guard<std::mutex> l(_m);
    if (_win) ANativeWindow_release(_win);
    _win = w;
    _geom_w = _geom_h = 0;
  }
  // DSP thread: copy and go
  void post(const Frame &f) {
    {
      std::lock_guard<std::mutex> l(_m);
      _w = f.width;
      _h = f.height;
      _pending.assign(f.data, f.data + static_cast<size_t>(f.width) * f.height * 4);
      _have = true;
    }
    _cv.notify_one();
  }

private:
  void loop() {
    std::vector<uint8_t> px;  // RGBX
    int w = 0, h = 0;
    for (;;) {
      ANativeWindow *win;
      {
        std::unique_lock<std::mutex> l(_m);
        _cv.wait(l, [&] { return _have || _stop; });
        if (_stop) return;
        px.swap(_pending);
        w = _w;
        h = _h;
        _have = false;
        win = _win;
        if (!win) continue;
        if (_geom_w != w || _geom_h != h) {
          ANativeWindow_setBuffersGeometry(win, w, h, WINDOW_FORMAT_RGBX_8888);
          _geom_w = w;
          _geom_h = h;
        }
        ANativeWindow_acquire(win);
      }
      ANativeWindow_Buffer buf;
      if (ANativeWindow_lock(win, &buf, nullptr) == 0) {
        const int rows = std::min(h, buf.height), cols = std::min(w, buf.width);
        for (int y = 0; y < rows; y++)
          std::memcpy(static_cast<uint32_t *>(buf.bits) + static_cast<size_t>(y) * buf.stride,
                      px.data() + static_cast<size_t>(y) * w * 4, static_cast<size_t>(cols) * 4);
        ANativeWindow_unlockAndPost(win);
      }
      ANativeWindow_release(win);
    }
  }
  std::mutex _m;
  std::condition_variable _cv;
  ANativeWindow *_win = nullptr;
  int _geom_w = 0, _geom_h = 0;
  std::vector<uint8_t> _pending;
  int _w = 0, _h = 0;
  bool _have = false, _stop = false;
  std::thread _th;
};

std::unique_ptr<Renderer> g_render;
std::unique_ptr<Radio> g_radio;
std::mutex g_api;  // serialises open/close against the other calls
}  // namespace

extern "C" {

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_init(JNIEnv *env, jclass, jstring log_path) {
  std::lock_guard<std::mutex> l(g_api);
  const char *p = env->GetStringUTFChars(log_path, nullptr);
  {
    std::lock_guard<std::mutex> lk(g_log_m);
    if (!g_log) g_log = std::fopen(p, "a");
  }
  env->ReleaseStringUTFChars(log_path, p);
  set_log_sink(file_sink);
  if (!g_render) g_render = std::make_unique<Renderer>();
  if (!g_radio) g_radio = std::make_unique<Radio>([](const Frame &f) { g_render->post(f); });
  KP_LOGI("kepyur native ready");
}

JNIEXPORT jstring JNICALL Java_com_kepyur_app_Native_usb3Switch(JNIEnv *env, jclass, jint fd) {
  std::string detail;
  const auto r = wsdr::request_usb3_fd(fd, &detail);
  std::string out = wsdr::usb3_switch_name(r);
  if (!detail.empty()) out += ": " + detail;
  KP_LOGI("usb3 switch: %s", out.c_str());
  return env->NewStringUTF(out.c_str());
}

JNIEXPORT jstring JNICALL Java_com_kepyur_app_Native_open(JNIEnv *env, jclass, jint fd) {
  std::lock_guard<std::mutex> l(g_api);
  try {
    g_radio->open(fd);
    return env->NewStringUTF("");
  } catch (const std::exception &e) {
    KP_LOGE("open failed: %s", e.what());
    return env->NewStringUTF(e.what()[0] ? e.what() : "open failed");
  }
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_close(JNIEnv *, jclass) {
  std::lock_guard<std::mutex> l(g_api);
  if (g_radio) g_radio->close();
}

JNIEXPORT jboolean JNICALL Java_com_kepyur_app_Native_superspeed(JNIEnv *, jclass) {
  return g_radio && g_radio->superspeed();
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_scan(JNIEnv *, jclass) {
  std::lock_guard<std::mutex> l(g_api);
  if (g_radio && g_radio->is_open()) g_radio->scan();
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_watch(JNIEnv *, jclass, jdouble hz) {
  std::lock_guard<std::mutex> l(g_api);
  if (g_radio && g_radio->is_open()) g_radio->watch(hz);
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_streamTest(JNIEnv *, jclass, jint rate_sel, jdouble seconds) {
  std::lock_guard<std::mutex> l(g_api);
  if (g_radio && g_radio->is_open()) g_radio->stream_test(rate_sel, seconds);
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_setWatchRate(JNIEnv *, jclass, jint rate_sel) {
  if (g_radio) g_radio->set_watch_rate(rate_sel);
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_setGain(JNIEnv *, jclass, jdouble db) {
  if (g_radio) g_radio->set_gain(db);
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_setComb(JNIEnv *, jclass, jboolean on) {
  if (g_radio) g_radio->set_comb(on);
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_record(JNIEnv *env, jclass, jdouble seconds, jstring prefix) {
  const char *p = env->GetStringUTFChars(prefix, nullptr);
  if (g_radio) g_radio->record(seconds, p);
  env->ReleaseStringUTFChars(prefix, p);
}

JNIEXPORT jstring JNICALL Java_com_kepyur_app_Native_status(JNIEnv *env, jclass) {
  const std::string s = g_radio ? g_radio->status_json() : std::string("{\"open\":false}");
  return env->NewStringUTF(s.c_str());
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_setSurface(JNIEnv *env, jclass, jobject surface) {
  if (!g_render) return;
  g_render->set_window(surface ? ANativeWindow_fromSurface(env, surface) : nullptr);
}

JNIEXPORT void JNICALL Java_com_kepyur_app_Native_log(JNIEnv *env, jclass, jstring msg) {
  const char *p = env->GetStringUTFChars(msg, nullptr);
  KP_LOGI("app: %s", p);
  env->ReleaseStringUTFChars(msg, p);
}

}  // extern "C"
