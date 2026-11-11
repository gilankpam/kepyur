# libusb (WebUSB backend) built with Emscripten from third_party/libusb (LIBUSB_SRC), without running its
# configure script. Installs libusb-1.0.a, libusb.h and libusb-1.0.pc so devourer's pkg-config finds it.
cmake_minimum_required(VERSION 3.21)
project(libusb_wasm C CXX)
if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "configure with emcmake")
endif()
set(LIBUSB_SRC "" CACHE PATH "libusb source tree (1.0.27 or newer)")
set(CMAKE_CXX_STANDARD 20)
set(S ${LIBUSB_SRC}/libusb)
add_library(usb-1.0 STATIC
  ${S}/core.c ${S}/descriptor.c ${S}/hotplug.c ${S}/io.c ${S}/strerror.c ${S}/sync.c
  ${S}/os/events_posix.c ${S}/os/threads_posix.c ${S}/os/emscripten_webusb.cpp fcntl_shim.c)
# WASMFS (the web build's file system) refuses O_NONBLOCK on libusb's event pipe: see fcntl_shim.c
set_source_files_properties(${S}/os/events_posix.c PROPERTIES COMPILE_DEFINITIONS "fcntl=wsdr_libusb_fcntl")
target_include_directories(usb-1.0 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${S} ${S}/os)
target_compile_options(usb-1.0 PRIVATE ${WSDR_EXC_FLAGS})
install(TARGETS usb-1.0 ARCHIVE DESTINATION lib)
install(FILES ${S}/libusb.h DESTINATION include/libusb-1.0)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/libusb-1.0.pc
"prefix=${CMAKE_INSTALL_PREFIX}
Name: libusb-1.0
Description: libusb (Emscripten/WebUSB)
Version: 1.0.29
Libs: -L\${prefix}/lib -lusb-1.0
Cflags: -I\${prefix}/include/libusb-1.0 -I\${prefix}/include
")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libusb-1.0.pc DESTINATION lib/pkgconfig)
