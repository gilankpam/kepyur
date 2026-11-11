# libusb for Windows (MinGW), built from third_party/libusb (v1.0.29) as a static library, with one change:
# bulk-IN pipes get WinUSB's RAW_IO policy. Without it WinUSB works through queued reads one at a time
# and leaves the pipe idle between them, so the chip stalls between C2H packets and the 40 MS/s
# stream misses its deadline. RAW_IO hands every queued read straight to the host controller. It requires
# each read to be a multiple of the max packet size (the streamer's 16 KiB reads are). Set
# KP_WINUSB_RAW_IO=0 in the environment to skip it.
# The patch is applied to a copy in the build tree; the submodule stays untouched.
set(_lu ${KEPYUR_ROOT}/third_party/libusb)
set(_lugen ${CMAKE_BINARY_DIR}/libusb-gen)
file(MAKE_DIRECTORY ${_lugen})

file(READ ${_lu}/libusb/os/windows_winusb.c _wu)
set(_anchor "		if (sub_api == SUB_API_LIBUSBK) {\n			if (!WinUSBX[sub_api].SetPipePolicy(winusb_handle, endpoint_address,\n				ISO_ALWAYS_START_ASAP")
string(FIND "${_wu}" "${_anchor}" _at)
if(_at LESS 0)
  message(FATAL_ERROR "libusb_windows.cmake: the RAW_IO patch anchor is gone from windows_winusb.c")
endif()
string(REPLACE "${_anchor}" "		if ((endpoint_address & LIBUSB_ENDPOINT_IN) && sub_api == SUB_API_WINUSB) { /* kepyur: RAW_IO */\n			const char *kp_raw = getenv(\"KP_WINUSB_RAW_IO\");\n			if (!kp_raw || kp_raw[0] != '0') {\n				if (!WinUSBX[sub_api].SetPipePolicy(winusb_handle, endpoint_address, RAW_IO, sizeof(UCHAR), &policy))\n					usbi_warn(HANDLE_CTX(dev_handle), \"failed to enable RAW_IO for endpoint %02X\", endpoint_address);\n				else\n					usbi_dbg(HANDLE_CTX(dev_handle), \"RAW_IO on for endpoint %02X\", endpoint_address);\n			}\n		}\n\n${_anchor}" _wu "${_wu}")
file(WRITE ${_lugen}/windows_winusb.c "${_wu}")

file(WRITE ${_lugen}/config.h
"#define DEFAULT_VISIBILITY __attribute__ ((visibility (\"default\")))
#define ENABLE_LOGGING 1
#define PLATFORM_WINDOWS 1
#define PRINTF_FORMAT(a, b) __attribute__ ((__format__ (__printf__, a, b)))
#define HAVE_STRUCT_TIMESPEC 1
#define _GNU_SOURCE 1
")

add_library(usb-1.0-static STATIC
  ${_lu}/libusb/core.c ${_lu}/libusb/descriptor.c ${_lu}/libusb/hotplug.c ${_lu}/libusb/io.c
  ${_lu}/libusb/strerror.c ${_lu}/libusb/sync.c
  ${_lu}/libusb/os/events_windows.c ${_lu}/libusb/os/threads_windows.c ${_lu}/libusb/os/windows_common.c
  ${_lu}/libusb/os/windows_usbdk.c ${_lugen}/windows_winusb.c)
target_include_directories(usb-1.0-static PRIVATE ${_lugen} ${_lu}/libusb/os)
target_include_directories(usb-1.0-static PUBLIC ${_lu}/libusb)
target_compile_options(usb-1.0-static PRIVATE -w)

# devourer finds libusb with pkg_check_modules(libusb-1.0): point it at a .pc with the headers only;
# the library is linked explicitly (native/CMakeLists.txt).
set(_pcdir ${CMAKE_BINARY_DIR}/pkgconfig)
file(MAKE_DIRECTORY ${_pcdir})
file(WRITE ${_pcdir}/libusb-1.0.pc
"Name: libusb-1.0\nDescription: libusb (kepyur, built in-tree for Windows)\nVersion: 1.0.29\nLibs:\nCflags: -I${_lu}/libusb\n")
set(ENV{PKG_CONFIG_LIBDIR} ${_pcdir})
set(ENV{PKG_CONFIG_PATH} ${_pcdir})
