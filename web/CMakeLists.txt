# kepyur web: native/dsp + native/radio + wifi-sdr (+ devourer) as WebAssembly, the adapter reached over
# WebUSB through libusb's Emscripten backend (web/libusb). Configure with emcmake: scripts/build-web.sh.
cmake_minimum_required(VERSION 3.21)
project(kepyur_web C CXX)
if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "configure with emcmake (scripts/build-web.sh)")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# JSPI (blocking libusb calls wait on WebUSB promises) needs wasm exceptions; -pthread: libusb's event loop and
# the radio's threads use shared memory, so the page must be cross-origin isolated (COOP/COEP).
set(KP_EXC -fwasm-exceptions -pthread)
add_compile_options(${KP_EXC} -msimd128)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_subdirectory(${ROOT}/third_party/wifi-sdr wifi-sdr EXCLUDE_FROM_ALL)
# the stream's bulk-IN reads queued from JavaScript (wifi-sdr bulkin_js.h; worker.js wsdrBulkStart/Stop): libusb's
# proxied transfers missed the chip's deadline at 20 MS/s (90.8 % of banks, 11 restarts in 20 s; this: 100 %, 0)
target_compile_definitions(wsdrcore PRIVATE WSDR_JS_BULKIN)

add_library(kepyur_dsp STATIC ${ROOT}/native/dsp/video.cpp ${ROOT}/native/dsp/spectrum.cpp ${ROOT}/native/dsp/log.cpp)
target_include_directories(kepyur_dsp PUBLIC ${ROOT}/native/dsp)
target_compile_options(kepyur_dsp PRIVATE -O3 -ffast-math)

set(LIBUSB_PREFIX "" CACHE PATH "where web/libusb installed libusb-1.0.a")
# usb_abort.cpp: WebUSB cannot cancel a transfer; a stopped stream's reads are aborted instead (--wrap)
add_executable(kepyur kepyur_web.cpp usb_abort.cpp ${ROOT}/native/radio/radio.cpp)
target_include_directories(kepyur PRIVATE ${ROOT}/native/radio)
target_compile_options(kepyur PRIVATE -O2 -Wall)
target_link_libraries(kepyur kepyur_dsp wsdrcore)
target_link_directories(kepyur PRIVATE ${LIBUSB_PREFIX}/lib)
# A fixed heap (growth + pthreads slows every heap access), a thread pool (the radio starts ~6), 1 MB stacks.
target_link_options(kepyur PRIVATE ${KP_EXC} -sJSPI -lembind -sENVIRONMENT=web,worker -sMODULARIZE -sEXPORT_ES6
  -sINITIAL_MEMORY=512MB -sSTACK_SIZE=1MB -sDEFAULT_PTHREAD_STACK_SIZE=1MB -sPTHREAD_POOL_SIZE=12
  -sEXPORT_NAME=createKepyur
  -Wl,--wrap=libusb_cancel_transfer -Wl,--wrap=libusb_submit_transfer
  -sEXPORTED_FUNCTIONS=_kp_init,_kp_cmd,_kp_frame_ack,_malloc,_free
  -sEXPORTED_RUNTIME_METHODS=stringToNewUTF8,HEAPU8,HEAP32)
set_target_properties(kepyur PROPERTIES SUFFIX ".js")
