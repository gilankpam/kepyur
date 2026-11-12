package com.kepyur.app

import android.annotation.SuppressLint
import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import org.json.JSONObject
import java.util.concurrent.Executors
import kotlin.math.abs

/**
 * kepyur: one screen. Video always fills the screen (4:3, centred); the pilot list docks on the right
 * while scanning and slides away when a pilot is picked; a swipe in from the right edge (or the handle)
 * brings it back.
 *
 * Test mode (for the first hardware check):  adb shell am start -n com.kepyur.app/.MainActivity --es mode usbtest
 */
class MainActivity : Activity() {
    private val tag = "kepyur"
    private val ui = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()
    private lateinit var usb: UsbManager

    private enum class State { NO_ADAPTER, PERMISSION, SWITCHING, OPENING, RUNNING, USB2, ERROR }
    private var state = State.NO_ADAPTER
    private var errorText = ""
    private var device: UsbDevice? = null
    private var conn: UsbDeviceConnection? = null
    private var expectReenumerate = false
    private var mode = "normal"
    private val testPlan = ArrayDeque<Pair<Int, Double>>()
    private var testRunning = false
    private val testResults = mutableListOf<String>()

    // views
    private lateinit var root: FrameLayout
    private lateinit var videoFrame: AspectFrame
    private lateinit var video: SurfaceView
    private lateinit var label: TextView
    private lateinit var centre: TextView
    private lateinit var panel: LinearLayout
    private lateinit var panelHeader: TextView
    private lateinit var pilotRows: LinearLayout
    private lateinit var handle: TextView
    private var panelShown = true
    private var lastLabelTouch = 0L
    private var lastPilotsJson = ""
    private var watchingHz = 0.0

    private val permAction = "com.kepyur.app.USB_PERMISSION"

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        usb = getSystemService(USB_SERVICE) as UsbManager
        mode = intent?.getStringExtra("mode") ?: "normal"
        Native.init(getExternalFilesDir(null)!!.absolutePath + "/kepyur.log")
        Native.log("start, mode=$mode, ${Build.MANUFACTURER} ${Build.MODEL} Android ${Build.VERSION.RELEASE}")
        buildUi()
        val f = IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(usbReceiver, f, RECEIVER_EXPORTED)
            registerReceiver(permReceiver, IntentFilter(permAction), RECEIVER_NOT_EXPORTED)
        } else {
            registerReceiver(usbReceiver, f)
            registerReceiver(permReceiver, IntentFilter(permAction))
        }
        ui.post(poll)
        connect()
        intent?.let { debugExtras(it) }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        intent.getStringExtra("mode")?.let { mode = it }
        if (intent.action == UsbManager.ACTION_USB_DEVICE_ATTACHED) connect()
        debugExtras(intent)
    }

    /** adb shell am start -n com.kepyur.app/.MainActivity --ei watchrate 1|2 --ef gain 30 --ef record 3 */
    private fun debugExtras(i: Intent) {
        if (i.hasExtra("gain")) {
            val g = i.getFloatExtra("gain", -1f).toDouble()
            Native.setGain(g)
            Native.log("debug: gain ${if (g < 0) "AGC" else "$g dB"}")
            if (watchingHz > 0 && state == State.RUNNING && !i.hasExtra("watchrate")) Native.watch(watchingHz)
        }
        if (i.hasExtra("watchrate")) {
            val r = i.getIntExtra("watchrate", 2)
            Native.setWatchRate(r)
            Native.log("debug: watch rate_sel $r")
            if (watchingHz > 0 && state == State.RUNNING) Native.watch(watchingHz)
        }
        if (i.hasExtra("record")) {
            val secs = i.getFloatExtra("record", 3f).toDouble()
            val name = "rec-" + java.text.SimpleDateFormat("HHmmss", java.util.Locale.US).format(java.util.Date())
            Native.record(secs, getExternalFilesDir(null)!!.absolutePath + "/" + name)
        }
    }

    override fun onResume() {
        super.onResume()
        hideSystemBars()
    }

    override fun onDestroy() {
        ui.removeCallbacksAndMessages(null)
        unregisterReceiver(usbReceiver)
        unregisterReceiver(permReceiver)
        worker.execute { Native.close() }
        conn?.close()
        super.onDestroy()
    }

    // ---------------------------------------------------------------------------------------- USB
    private fun ours(d: UsbDevice) = // T4U v3, T3U, T3U Plus; Realtek's own 8812BU id
        (d.vendorId == 0x2357 && d.productId in setOf(0x0115, 0x012d, 0x0138)) ||
            (d.vendorId == 0x0bda && d.productId == 0xb812)

    private fun connect() {
        if (state == State.OPENING || state == State.RUNNING) return
        val d = usb.deviceList.values.firstOrNull { ours(it) }
        if (d == null) {
            setState(if (expectReenumerate) State.SWITCHING else State.NO_ADAPTER)
            return
        }
        device = d
        if (!usb.hasPermission(d)) {
            setState(State.PERMISSION)
            val pi = PendingIntent.getBroadcast(
                this, 0, Intent(permAction).setPackage(packageName),
                PendingIntent.FLAG_MUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
            )
            usb.requestPermission(d, pi)
            return
        }
        val c = usb.openDevice(d)
        if (c == null) {
            fail("Could not open the adapter (is another app using it?)")
            return
        }
        conn = c
        val fd = c.fileDescriptor
        Native.log("adapter ${d.deviceName} ${"%04x:%04x".format(d.vendorId, d.productId)} fd=$fd")
        setState(State.OPENING)
        worker.execute {
            val sw = Native.usb3Switch(fd)
            if (sw.startsWith("requested")) {
                // the adapter drops off the bus and comes back as SuperSpeed: a new device, a new permission
                ui.post {
                    expectReenumerate = true
                    c.close(); conn = null
                    setState(State.SWITCHING)
                    ui.postDelayed({ if (state == State.SWITCHING) connect() }, 4000)
                }
                return@execute
            }
            val err = Native.open(fd)
            ui.post {
                expectReenumerate = false
                when {
                    err.isNotEmpty() -> fail("Radio did not start: $err")
                    !Native.superspeed() -> setState(State.USB2)
                    else -> {
                        setState(State.RUNNING)
                        startWork()
                    }
                }
            }
        }
    }

    private fun startWork() {
        if (mode == "usbtest") {
            testPlan.clear(); testResults.clear()
            testPlan.add(1 to 60.0)   // 40 MS/s for 60 s
            testPlan.add(2 to 60.0)   // 20 MS/s for 60 s
            nextTest()
        } else {
            Native.scan()
            showPanel(true)
        }
    }

    private fun nextTest() {
        val t = testPlan.removeFirstOrNull()
        if (t == null) {
            testRunning = false
            Native.log("usbtest: all done: ${testResults.joinToString(" | ")}")
            Native.log("usbtest: now decoding for the rest of the session (scan, then watch)")
            mode = "normal"
            Native.scan()
            showPanel(true)
            return
        }
        testRunning = true
        Native.log("usbtest: rate_sel ${t.first} for ${t.second} s")
        Native.streamTest(t.first, t.second)
    }

    private fun closeRadio() {
        worker.execute { Native.close() }
        conn?.close(); conn = null
    }

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(ctx: Context, i: Intent) {
            val d: UsbDevice? = if (Build.VERSION.SDK_INT >= 33)
                i.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
            else @Suppress("DEPRECATION") i.getParcelableExtra(UsbManager.EXTRA_DEVICE)
            if (d == null || !ours(d)) return
            when (i.action) {
                UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                    Native.log("adapter detached${if (expectReenumerate) " (USB 3 switch)" else ""}")
                    if (state == State.RUNNING || state == State.OPENING || state == State.USB2) closeRadio()
                    if (!expectReenumerate) setState(State.NO_ADAPTER)
                }
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                    Native.log("adapter attached")
                    if (state != State.RUNNING && state != State.OPENING) {
                        state = State.NO_ADAPTER
                        connect()
                    }
                }
            }
        }
    }

    private val permReceiver = object : BroadcastReceiver() {
        override fun onReceive(ctx: Context, i: Intent) {
            val ok = i.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
            Native.log("usb permission ${if (ok) "granted" else "denied"}")
            if (ok) {
                state = State.NO_ADAPTER
                connect()
            } else fail("USB permission denied. Unplug and replug the adapter to try again.")
        }
    }

    private fun fail(msg: String) {
        errorText = msg
        Native.log("error: $msg")
        setState(State.ERROR)
    }

    // ---------------------------------------------------------------------------------------- polling
    private val poll = object : Runnable {
        override fun run() {
            if (state == State.RUNNING || state == State.USB2) render(JSONObject(Native.status()))
            if (System.currentTimeMillis() - lastLabelTouch > 4000 && label.alpha > 0f && !panelShown)
                label.animate().alpha(0f).setDuration(400).start()
            ui.postDelayed(this, 250)
        }
    }

    private fun render(s: JSONObject) {
        val m = s.optString("mode")
        val video = s.optJSONObject("video")
        val locked = video?.optBoolean("locked") ?: false
        watchingHz = if (m == "watch") s.optDouble("watch_hz") else watchingHz
        // centre message
        val msg = when {
            state == State.USB2 -> "The adapter is running at USB 2.\nVideo needs USB 3: use the phone's USB-C port directly with a USB 3 OTG adapter or cable."
            m == "test" -> {
                val t = s.optJSONObject("test")
                "USB test  " + (t?.let { "%.0f MS/s  %.0f s  banks %d / %d  gaps %d  restarts %d  dropped %d".format(
                    it.optDouble("rate_msps"), it.optDouble("seconds"), it.optLong("banks"), it.optLong("expected"),
                    it.optLong("seq_gaps"), it.optLong("restarts"), it.optLong("queue_dropped")) } ?: "starting…")
            }
            m == "scan" -> "Scanning…  ${s.optInt("scan_step")}/${s.optInt("scan_steps")}"
            m == "watch" && !locked -> "Searching for the picture…"
            m == "idle" && s.optJSONArray("pilots")?.length() == 0 -> "No pilots found.\nOpen the list and tap Rescan."
            else -> ""
        }
        centre.text = msg
        centre.visibility = if (msg.isEmpty()) View.GONE else View.VISIBLE
        // test sequencing
        if (testRunning && m == "idle") {
            s.optJSONObject("test")?.let { if (it.optBoolean("done")) { testResults.add(it.toString()); nextTest() } }
        }
        // label
        val p = currentPilot(s)
        val lockTxt = if (m == "watch") (if (locked) "● locked" else "○ searching") else m
        label.text = buildString {
            if (p != null) append(p.optString("name").ifEmpty { "—" }).append(" · ")
            if (m == "watch") append("%.0f MHz · ".format(watchingHz / 1e6))
            append(lockTxt)
            if (m == "watch" && locked) append(" · %.0f fps".format(video?.optDouble("fps") ?: 0.0))
            val load = s.optDouble("dsp_load")
            val dropped = s.optLong("dropped")
            if (load > 0.8 || dropped > 0) append("  [cpu %.0f%%, dropped %d]".format(load * 100, dropped))
        }
        label.setTextColor(if (locked) Color.WHITE else Color.rgb(255, 200, 120))
        // pilot list
        val pilots = s.optJSONArray("pilots")
        val pj = (pilots?.toString() ?: "") + m + watchingHz
        panelHeader.text = if (m == "scan") "Pilots   scanning ${s.optInt("scan_step")}/${s.optInt("scan_steps")}" else "Pilots"
        if (pj != lastPilotsJson) {
            lastPilotsJson = pj
            pilotRows.removeAllViews()
            if (pilots != null) for (i in 0 until pilots.length()) pilotRows.addView(pilotRow(pilots.getJSONObject(i), m))
        }
    }

    private fun currentPilot(s: JSONObject): JSONObject? {
        val pilots = s.optJSONArray("pilots") ?: return null
        for (i in 0 until pilots.length()) {
            val p = pilots.getJSONObject(i)
            if (abs(p.optDouble("mhz") * 1e6 - watchingHz) < 4e6) return p
        }
        return null
    }

    // ---------------------------------------------------------------------------------------- UI
    private fun dp(v: Float) = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, resources.displayMetrics)
    private fun dpi(v: Float) = dp(v).toInt()

    private fun setState(s: State) {
        state = s
        val msg = when (s) {
            State.NO_ADAPTER -> "Plug in the adapter."
            State.PERMISSION -> "Allow kepyur to use the adapter."
            State.SWITCHING -> "Switching the adapter to USB 3…"
            State.OPENING -> "Starting the radio…"
            State.ERROR -> errorText
            State.USB2, State.RUNNING -> centre.text.toString()
        }
        centre.text = msg
        centre.visibility = if (msg.isEmpty()) View.GONE else View.VISIBLE
        if (s != State.RUNNING) {
            label.text = ""
            lastPilotsJson = ""
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    private fun buildUi() {
        root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }
        videoFrame = AspectFrame(this, 4f / 3f)
        video = SurfaceView(this)
        video.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(h: SurfaceHolder) = Native.setSurface(h.surface)
            override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, ht: Int) = Native.setSurface(h.surface)
            override fun surfaceDestroyed(h: SurfaceHolder) = Native.setSurface(null)
        })
        videoFrame.addView(video, FrameLayout.LayoutParams(-1, -1))
        root.addView(videoFrame, FrameLayout.LayoutParams(-1, -1, Gravity.CENTER))

        label = TextView(this).apply {
            setTextColor(Color.WHITE)
            textSize = 15f
            setPadding(dpi(10f), dpi(5f), dpi(10f), dpi(5f))
            background = rounded(Color.argb(150, 0, 0, 0), 8f)
        }
        root.addView(label, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.START).apply {
            setMargins(dpi(16f), dpi(12f), 0, 0)
        })

        centre = TextView(this).apply {
            setTextColor(Color.WHITE)
            textSize = 18f
            gravity = Gravity.CENTER
            setPadding(dpi(18f), dpi(12f), dpi(18f), dpi(12f))
            background = rounded(Color.argb(170, 0, 0, 0), 10f)
        }
        root.addView(centre, FrameLayout.LayoutParams(-2, -2, Gravity.CENTER))

        // pilot panel
        panel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.argb(225, 12, 15, 19))
            setPadding(0, dpi(8f), 0, dpi(8f))
            isClickable = true
        }
        panelHeader = TextView(this).apply {
            setTextColor(Color.WHITE); textSize = 16f
            setPadding(dpi(14f), dpi(4f), dpi(14f), dpi(8f))
            text = "Pilots"
        }
        panel.addView(panelHeader)
        pilotRows = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        panel.addView(ScrollView(this).apply { addView(pilotRows) }, LinearLayout.LayoutParams(-1, 0, 1f))
        val buttons = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; setPadding(dpi(8f), 0, dpi(8f), 0) }
        buttons.addView(Button(this).apply {
            text = "Rescan"; setOnClickListener { if (state == State.RUNNING) Native.scan() }
        }, LinearLayout.LayoutParams(0, -2, 1f))
        buttons.addView(Button(this).apply {
            text = "USB test"; setOnClickListener { if (state == State.RUNNING) { mode = "usbtest"; startWork() } }
        }, LinearLayout.LayoutParams(0, -2, 1f))
        panel.addView(buttons)
        root.addView(panel, FrameLayout.LayoutParams(0, -1, Gravity.END))

        handle = TextView(this).apply {
            text = "‹"; textSize = 28f; setTextColor(Color.argb(170, 255, 255, 255))
            gravity = Gravity.CENTER
            background = rounded(Color.argb(120, 0, 0, 0), 8f)
            setOnClickListener { showPanel(true) }
        }
        root.addView(handle, FrameLayout.LayoutParams(dpi(28f), dpi(64f), Gravity.END or Gravity.CENTER_VERTICAL))

        // tap: show the label (or hide the list); swipe in from the right edge: show the list
        var downX = 0f; var downY = 0f
        root.setOnTouchListener { v, e ->
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> { downX = e.x; downY = e.y }
                MotionEvent.ACTION_UP -> {
                    val dx = e.x - downX; val dy = e.y - downY
                    if (downX > v.width - dp(48f) && dx < -dp(60f) && abs(dy) < abs(dx)) showPanel(true)
                    else if (abs(dx) < dp(10f) && abs(dy) < dp(10f)) {
                        if (panelShown && state == State.RUNNING) showPanel(false)
                        lastLabelTouch = System.currentTimeMillis()
                        label.animate().alpha(1f).setDuration(150).start()
                    }
                }
            }
            true
        }
        root.addOnLayoutChangeListener { _, _, _, r, _, _, _, oldR, _ -> if (r != oldR) layoutPanel() }
        setContentView(root)
        setState(State.NO_ADAPTER)
    }

    private fun layoutPanel() {
        val w = (root.width * 0.38f).toInt().coerceAtLeast(dpi(260f))
        panel.layoutParams = (panel.layoutParams as FrameLayout.LayoutParams).apply { width = w }
        panel.translationX = if (panelShown) 0f else w.toFloat()
        handle.visibility = if (panelShown) View.GONE else View.VISIBLE
    }

    private fun showPanel(show: Boolean) {
        panelShown = show
        val w = panel.width.toFloat().takeIf { it > 0 } ?: (root.width * 0.38f)
        panel.animate().translationX(if (show) 0f else w).setDuration(220).start()
        handle.visibility = if (show) View.GONE else View.VISIBLE
        if (!show) lastLabelTouch = System.currentTimeMillis()
        label.animate().alpha(1f).setDuration(150).start()
    }

    private fun pilotRow(p: JSONObject, m: String): View {
        val mhz = p.optDouble("mhz")
        val selected = m == "watch" && abs(mhz * 1e6 - watchingHz) < 4e6
        val snr = p.optDouble("snr")
        val bars = when { snr >= 35 -> 4; snr >= 25 -> 3; snr >= 15 -> 2; else -> 1 }
        val age = p.optLong("age_s")
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dpi(14f), dpi(12f), dpi(14f), dpi(12f))
            if (selected) setBackgroundColor(Color.rgb(20, 50, 77))
            setOnClickListener {
                Native.watch(mhz * 1e6)
                watchingHz = mhz * 1e6
                lastPilotsJson = ""
                showPanel(false)
            }
        }
        fun tv(t: String, size: Float, color: Int, w: Float) = TextView(this).apply {
            text = t; textSize = size; setTextColor(color)
        }.also { row.addView(it, LinearLayout.LayoutParams(if (w > 0) dpi(w) else 0, -2, if (w > 0) 0f else 1f)) }
        tv(p.optString("name").ifEmpty { "—" }, 18f, Color.WHITE, 46f)
        tv("%.0f MHz".format(mhz), 15f, Color.rgb(220, 220, 220), 86f)
        tv(p.optString("std"), 12f, Color.rgb(170, 170, 170), 0f)
        if (age > 15 && !selected) tv("${age}s ago", 11f, Color.rgb(140, 140, 140), 56f)
        row.addView(signalBars(bars), LinearLayout.LayoutParams(dpi(30f), dpi(20f)))
        return row
    }

    /** Four rising bars, [n] of them lit. */
    private fun signalBars(n: Int): View {
        val box = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.BOTTOM }
        for (i in 0 until 4) {
            box.addView(View(this).apply {
                setBackgroundColor(if (i < n) Color.rgb(76, 175, 80) else Color.rgb(58, 63, 69))
            }, LinearLayout.LayoutParams(dpi(5f), dpi(5f + 5f * i)).apply { marginEnd = dpi(2.5f) })
        }
        return box
    }

    private fun rounded(color: Int, r: Float) = GradientDrawable().apply { setColor(color); cornerRadius = dp(r) }

    private fun hideSystemBars() {
        window.insetsController?.let {
            it.hide(WindowInsets.Type.systemBars())
            it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }
}

/** Keeps its child at a fixed aspect ratio, as large as fits. */
class AspectFrame(ctx: Context, private val aspect: Float) : FrameLayout(ctx) {
    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        val h = MeasureSpec.getSize(heightMeasureSpec)
        var cw = w
        var ch = (w / aspect).toInt()
        if (ch > h) { ch = h; cw = (h * aspect).toInt() }
        super.onMeasure(MeasureSpec.makeMeasureSpec(cw, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(ch, MeasureSpec.EXACTLY))
    }
}
