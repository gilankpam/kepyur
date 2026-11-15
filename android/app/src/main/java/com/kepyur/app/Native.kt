package com.kepyur.app

import android.view.Surface

/** The native radio (native/jni/kepyur_jni.cpp). */
object Native {
    init { System.loadLibrary("kepyur") }

    @JvmStatic external fun init(logPath: String)
    /** USB 2 -> 3 switch on this fd: "requested" means the adapter is re-enumerating now. */
    @JvmStatic external fun usb3Switch(fd: Int): String
    /** Blocking (~5 s). Returns "" on success, else a message. */
    @JvmStatic external fun open(fd: Int): String
    @JvmStatic external fun close()
    @JvmStatic external fun superspeed(): Boolean
    @JvmStatic external fun scan()
    @JvmStatic external fun watch(hz: Double)
    @JvmStatic external fun streamTest(rateSel: Int, seconds: Double)
    /** Debug: the rate watch uses (2 = 20 MS/s, 1 = 40 MS/s), from the next watch on. */
    @JvmStatic external fun setWatchRate(rateSel: Int)
    /** Debug: gain from the next tune on: < 0 = the chip's AGC, else fixed gain (dB). */
    @JvmStatic external fun setGain(db: Double)
    /** NTSC chroma comb (fewer rainbows on OSD text, ~15 % more decode CPU), from the next watch on. */
    @JvmStatic external fun setComb(on: Boolean)
    /** Debug: save the next [seconds] of watched IQ as [prefix].cs16 + .json. */
    @JvmStatic external fun record(seconds: Double, prefix: String)
    @JvmStatic external fun status(): String
    @JvmStatic external fun setSurface(surface: Surface?)
    @JvmStatic external fun log(msg: String)
}
