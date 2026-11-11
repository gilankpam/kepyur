// SPDX-License-Identifier: GPL-2.0-only
// Transfer cancellation for the web build (linked with --wrap=libusb_cancel_transfer and
// --wrap=libusb_submit_transfer, wasm/CMakeLists.txt).
//
// WebUSB cannot cancel one transfer, and libusb's WebUSB backend makes cancel a no-op: a stream that
// stops (retune, rate change) leaves its queued bulk-IN reads posted in the browser, where they swallow
// the next run's first packets (framing faults, then "stream keeps failing"). Releasing the interface
// aborts every pending transfer on it (WebUSB spec; ~1 ms in Chrome, nothing is sent to the chip), and
// claiming it again restores it. So the first bulk-IN cancel of a batch on a handle (the stream's
// reader cancels its whole queue at once) does release + claim; the cancelled transfers then complete
// at once, as they would natively. A submit on the handle ends the batch.
//
// Interface 0: the 8812BU / 8812CU have one interface (devourer's find_wifi_interface).
#include <libusb.h>

#include <mutex>
#include <set>

extern "C" int __real_libusb_cancel_transfer(libusb_transfer *t);
extern "C" int __real_libusb_submit_transfer(libusb_transfer *t);

namespace {
std::mutex g_m;
std::set<libusb_device_handle *> g_aborted; // handles whose pending transfers were aborted since their last submit
} // namespace

extern "C" int __wrap_libusb_submit_transfer(libusb_transfer *t) {
  {
    std::lock_guard<std::mutex> l(g_m);
    g_aborted.erase(t->dev_handle);
  }
  return __real_libusb_submit_transfer(t);
}

extern "C" int __wrap_libusb_cancel_transfer(libusb_transfer *t) {
  const int rc = __real_libusb_cancel_transfer(t);
  if (rc != 0 || t->type != LIBUSB_TRANSFER_TYPE_BULK || !(t->endpoint & LIBUSB_ENDPOINT_IN))
    return rc;
  {
    std::lock_guard<std::mutex> l(g_m);
    if (!g_aborted.insert(t->dev_handle).second)
      return rc; // this batch was aborted already
  }
  libusb_release_interface(t->dev_handle, 0);
  libusb_claim_interface(t->dev_handle, 0);
  return rc;
}
