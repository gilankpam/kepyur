/* libusb config.h for the Emscripten (WebUSB) backend, written by hand: what libusb's configure
 * produces for --host=wasm32-unknown-emscripten (configure.ac: platform posix, no pipe2). */
#define DEFAULT_VISIBILITY __attribute__((visibility("default")))
#define PRINTF_FORMAT(a, b) __attribute__((__format__(__printf__, a, b)))
#define ENABLE_LOGGING 1
#define PLATFORM_POSIX 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_NFDS_T 1
#define HAVE_SYS_TIME_H 1
#define HAVE_PTHREAD_CONDATTR_SETCLOCK 1
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
