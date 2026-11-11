/* SPDX-License-Identifier: GPL-2.0-only
 * fcntl for libusb's events_posix.c only (wasm/libusb/CMakeLists.txt maps it there): WASMFS accepts only
 * O_APPEND in F_SETFL and fails libusb_init when libusb makes its event pipe O_NONBLOCK. On Emscripten
 * libusb waits on an atomic, not on the pipe (events_posix.c, em_libusb_wait), and reads the pipe only
 * after writing to it, so a blocking pipe is harmless: that one failure is taken as success. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>

int wsdr_libusb_fcntl(int fd, int cmd, ...) {
  va_list ap;
  va_start(ap, cmd);
  const long arg = va_arg(ap, long);
  va_end(ap);
  const int r = fcntl(fd, cmd, arg);
  if (r == -1 && cmd == F_SETFL && errno == EINVAL && (arg & O_NONBLOCK))
    return 0;
  return r;
}
