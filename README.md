# kepyur

Android analog FPV receiver for spectating. A TP-Link Archer T4U v3 (RTL8812BU) plugged into the phone
streams 40 MS/s raw IQ through [wifi-sdr](https://github.com/snokvist/wifi-sdr) (fork:
`gilankpam/wifi-sdr`, branch `kepyur-android`) and [devourer](https://github.com/OpenIPC/devourer); kepyur
scans the 5.8 GHz bands for analog video, lists the pilots, and decodes the picked one in colour (NTSC and PAL; grey when the colour burst is too weak).

The latest APK from `main` is the rolling **main** pre-release (tag `main-latest`), rebuilt by CI
(`.github/workflows/android.yml`) on every push to `main`. It is a debug build signed with CI's key: if a
locally built kepyur is installed, uninstall it first (`adb uninstall com.kepyur.app`).

## Layout

- `native/dsp/` — video decoder (FM discriminator, sync, fields), spectrum + pilot finding. No hardware.
- `native/radio/` — the adapter (wifi-sdr `Receiver`) + a DSP thread: scan, watch, stream test.
- `native/jni/` — the app's native side; frames go straight to the video Surface.
- `native/tools/` — desktop: `kp_replay` (recordings), `kp_live` (adapter on this PC), `kp_sim` (kp_live
  against a recording played back as a tunable adapter).
- `android/` — the app (Kotlin, plain Views). `scripts/` — build and phone helpers.

## Build

Everything comes from `shell.nix`:

```sh
git clone https://github.com/gilankpam/kepyur && cd kepyur
git submodule update --init third_party/libusb third_party/wifi-sdr
git -C third_party/wifi-sdr submodule update --init external/devourer
nix-shell
scripts/build-desktop.sh      # build/desktop/kp_replay, kp_live, kp_sim
scripts/build-android.sh      # android/app/build/outputs/apk/debug/app-debug.apk
```

### Windows (desktop tools)

`kp_replay`, `kp_live` and `kp_sim` also build natively on Windows with MSYS2:

1. Install MSYS2 (`winget install MSYS2.MSYS2`), then in an MSYS2 UCRT64 shell:
   ```sh
   pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,libusb,pkgconf} vim make texinfo bison flex
   ```
2. mipsel binutils (MSYS2 has no package; wifi-sdr assembles the adapter's on-chip streamer with it):
   ```sh
   curl -LO https://ftp.gnu.org/gnu/binutils/binutils-2.44.tar.xz && tar xf binutils-2.44.tar.xz
   mkdir b && cd b && ../binutils-2.44/configure --target=mipsel-linux-gnu --prefix=/opt/mipsel \
     --disable-nls --disable-werror --disable-gdb --disable-gprof --disable-gprofng --disable-sim
   make -j8 && make install
   ```
3. Bind **WinUSB** to the adapter with [Zadig](https://zadig.akeo.ie) (Options → List All Devices). It
   then no longer works as a Wi-Fi card on that PC.
4. `scripts/build-windows.sh` (Git Bash or MSYS2) → `build/windows/*.exe` with the DLLs they need.

`kp_live` switches a cold-plugged 8812BU to USB 3 itself (Windows has no rtw88 to do it); `--no-usb3`
skips that. Watching runs at 40 MS/s (`--watch-rate 2` for 20 MS/s). Windows' USB stack stalls now and
then, so there the stream keeps banks that ship late or come back short (`late_banks` / `damaged_banks` in
the status) instead of restarting, and posts 2048 transfers. Expect a restart every ~10 s. Use a USB 3 port, not a hub. Video (cmd, Git Bash or PowerShell 7.4+; Windows PowerShell 5.1
corrupts binary pipes):

```bat
scripts\kp-live.cmd --scan           :: scan, watch the strongest pilot (ffplay window; log: build\windows\kp_live.log)
scripts\kp-live.cmd --watch 5848     :: straight to one frequency (MHz)
build\windows\kp_live.exe --scan --frames | ffplay -f rawvideo -pixel_format rgb0 -video_size 720x480 -framerate 60 -fflags nobuffer -i -
```

## On the phone

The adapter takes the USB-C port, so adb runs over Wi-Fi (Developer options → Wireless debugging):

```sh
scripts/phone.sh pair 192.168.1.23:37000 123456   # once ("Pair device with pairing code")
scripts/phone.sh connect 192.168.1.23:41000       # the port on the Wireless debugging screen
scripts/phone.sh install
scripts/phone.sh usbtest    # 60 s at 40 MS/s + 60 s at 20 MS/s, then scan/watch; logs -> logs/
scripts/phone.sh run        # normal start
scripts/phone.sh logs NAME  # pull kepyur.log + logcat
```

## Desktop bench

```sh
build/desktop/kp_replay ../vtx-spike/rec/demo.cs16            # lock numbers
build/desktop/kp_replay ../vtx-spike/rec/r4.cs16 --cnr 8      # with noise added (CNR in an 18 MHz channel)
build/desktop/kp_replay ../vtx-spike/rec/r4.cs16 --frames | ffplay -f rawvideo -pixel_format rgb0 -video_size 720x480 -i -
KP_SIM_FILE=../vtx-spike/rec/r4.cs16 build/desktop/kp_sim --scan   # scan -> watch without hardware
build/desktop/kp_live --scan --frames | ffplay -f rawvideo -pixel_format rgb0 -video_size 720x480 -framerate 60 -fflags nobuffer -i -
```

Measured on the spike's recordings (R4 NTSC VTX on the bench): 96.5 % of lines locked, no lock losses
over 10 s; the picture holds to about 5 dB CNR (it gets snowy, it doesn't drop).

License: GPL-2.0 (wifi-sdr is GPL-2.0-only).
