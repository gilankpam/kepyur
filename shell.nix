# kepyur build environment: everything the desktop harness, the Android app and the web version need.
#   nix-shell                   # then: scripts/build-desktop.sh, scripts/build-android.sh, scripts/build-web.sh
#   nix-shell --arg web true    # the web version only (no Android SDK/NDK: the Pages job)
{ web ? false }:
let
  # pinned (the same nixpkgs as the dev machine, NixOS 25.11.20260408) so CI builds with the same toolchain
  nixpkgs = fetchTarball {
    url = "https://github.com/NixOS/nixpkgs/archive/b5d476ea3060e8e5a804609137458c695a90fc0e.tar.gz";
    sha256 = "0bcj93n4sxr8sfd72g7fkxkw2yvi94022341vlampwyjvrlb9zpv";
  };
  pkgs = import nixpkgs {
    config.allowUnfree = true;
    config.android_sdk.accept_license = true;
  };
  ndkVersion = "28.2.13676358";
  android = pkgs.androidenv.composeAndroidPackages {
    platformVersions = [ "35" ];
    buildToolsVersions = [ "35.0.1" ];
    includeNDK = true;
    ndkVersions = [ ndkVersion ];
    cmakeVersions = [ "3.31.6" ];
    includeEmulator = false;
    includeSystemImages = false;
  };
  sdk = android.androidsdk;
  # wifi-sdr assembles the 8812BU's on-chip streamer (MIPS16e2) at build time and embeds it with xxd
  mips = pkgs.pkgsCross.mipsel-linux-gnu.buildPackages.binutils;
  # the web version: emscripten 4.0.12 (WebAssembly, JSPI, pthreads) and Node for its local server
  webTools = [ pkgs.emscripten pkgs.nodejs ];
  common = [ pkgs.cmake pkgs.ninja pkgs.pkg-config mips pkgs.xxd ];
  mipsEnv = { KEPYUR_MIPS_PREFIX = "${mips}/bin/mipsel-unknown-linux-gnu-"; };
in
if web then
  pkgs.mkShell ({ packages = common ++ webTools; } // mipsEnv)
else
  pkgs.mkShell ({
    packages = common ++ webTools ++ [
      sdk
      pkgs.jdk17
      pkgs.gradle
      pkgs.libusb1
      pkgs.ffmpeg
      (pkgs.python3.withPackages (ps: [ ps.numpy ps.scipy ps.matplotlib ps.pillow ]))
    ];
    ANDROID_HOME = "${sdk}/libexec/android-sdk";
    ANDROID_SDK_ROOT = "${sdk}/libexec/android-sdk";
    ANDROID_NDK_ROOT = "${sdk}/libexec/android-sdk/ndk/${ndkVersion}";
    KEPYUR_NDK_VERSION = ndkVersion;
    JAVA_HOME = "${pkgs.jdk17}";
    # aapt2 from Maven is a dynamically linked binary that NixOS cannot run; use the SDK's
    GRADLE_OPTS = "-Dorg.gradle.project.android.aapt2FromMavenOverride=${sdk}/libexec/android-sdk/build-tools/35.0.1/aapt2";
  } // mipsEnv)
