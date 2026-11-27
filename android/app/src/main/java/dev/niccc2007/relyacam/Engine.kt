package dev.niccc2007.relyacam

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.ImageFormat
import android.graphics.SurfaceTexture
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.media.ImageReader
import android.os.BatteryManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.util.Log
import android.util.Size
import android.view.Surface
import androidx.core.content.ContextCompat
import dev.niccc2007.relyacam.camera.CameraCatalog
import dev.niccc2007.relyacam.camera.CameraController
import dev.niccc2007.relyacam.camera.CameraEvents
import dev.niccc2007.relyacam.camera.CameraInfo
import dev.niccc2007.relyacam.camera.DeviceOrientation
import dev.niccc2007.relyacam.camera.EncoderSupport
import dev.niccc2007.relyacam.camera.Outputs
import dev.niccc2007.relyacam.camera.Purpose
import dev.niccc2007.relyacam.camera.Tilt
import dev.niccc2007.relyacam.camera.Upright
import dev.niccc2007.relyacam.protocol.CamCaps
import dev.niccc2007.relyacam.protocol.CamState
import dev.niccc2007.relyacam.protocol.Caps
import dev.niccc2007.relyacam.protocol.Cmd
import dev.niccc2007.relyacam.protocol.Discovery
import dev.niccc2007.relyacam.protocol.Grid
import dev.niccc2007.relyacam.protocol.Hello
import dev.niccc2007.relyacam.protocol.MicInput
import dev.niccc2007.relyacam.protocol.Mode
import dev.niccc2007.relyacam.protocol.Pairing
import dev.niccc2007.relyacam.protocol.Welcome
import dev.niccc2007.relyacam.protocol.WhiteBalance
import dev.niccc2007.relyacam.qr.Luma
import dev.niccc2007.relyacam.qr.QrDecode
import dev.niccc2007.relyacam.stream.AudioEncoder
import dev.niccc2007.relyacam.stream.Finder
import dev.niccc2007.relyacam.stream.Fx
import dev.niccc2007.relyacam.stream.Link
import dev.niccc2007.relyacam.stream.LinkEvents
import dev.niccc2007.relyacam.stream.MicSource
import dev.niccc2007.relyacam.stream.Mics
import dev.niccc2007.relyacam.stream.Reach
import dev.niccc2007.relyacam.stream.ReachException
import dev.niccc2007.relyacam.stream.VideoEncoder
import dev.niccc2007.relyacam.update.Release
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

enum class Screen { SCAN, CONNECTING, LIVE, UNREACHABLE, WAITING, GET_PLUGIN }

/** A failed connection, worded for a person (AGENTS.md rule 7). */
data class Problem(val reach: Reach?, val message: String, val pairing: Pairing?, val sameNetwork: Boolean)

data class UiState(
    val screen: Screen = Screen.SCAN,
    val cams: List<CameraInfo> = emptyList(),
    val cam: CameraInfo? = null,
    val zoom: Double = 1.0,
    val torch: Boolean = false,
    val mic: Boolean = true,
    val micAllowed: Boolean = false,
    val cameraAllowed: Boolean = false,
    val mode: String = "",
    val codec: String = "h264",
    val codecs: List<String> = emptyList(),
    val rot: Int = 0,
    val mirror: Boolean = false,
    val flip: Boolean = false,
    val lock: Boolean = false,
    val pcName: String = "",
    val transport: String = "",
    val reconnecting: Boolean = false,
    val problem: Problem? = null,
    val recent: List<SavedPc> = emptyList(),
    val black: Boolean = false,
    val temp: Double = 0.0,
    val battery: Int = -1,
    val rttMs: Long = 0,
    val kbps: Int = 0,
    val update: Release? = null,
    val notice: String? = null,
    val previewW: Int = 1280,
    val previewH: Int = 720,
    /** The phone's inputs right now, the one in use, its clean-up effects and which are on. */
    val mics: List<MicInput> = emptyList(),
    val micIn: String = "",
    val clean: List<String> = emptyList(),
    val ns: Boolean = false,
    val aec: Boolean = false,
    val agc: Boolean = false,
    /** Camera off, mic still streaming. */
    val micOnly: Boolean = false,
    /** Focus and exposure locked; brightness in the camera's own EV steps; white balance. */
    val aeLock: Boolean = false,
    val evIndex: Int = 0,
    val wb: String = WhiteBalance.AUTO,
    /** The photo size a snapshot takes now ("4096x3072"), or empty when this session has none. */
    val snap: String = "",
    /** Where the rule-of-thirds grid shows; the phone owns it so both screens agree. */
    val grid: Grid = Grid(),
    /** The level line (phone only, kept on the phone). */
    val levelLine: Boolean = false,
) {
    /** Brightness in EV for the current camera. */
    val ev: Double get() = evIndex * (cam?.evStep ?: 0.0)
}

/** The mic meter: peak of the last ~50 ms (0..1), and CLIP, held for a second after it happens. */
data class MicLevel(val peak: Float = 0f, val clip: Boolean = false)

/**
 * Everything the app does, in one place: scanning, connecting, streaming and applying requests.
 * The phone owns the camera's state, and a request from the PC and a tap on the phone go
 * through the same [apply], so both sides always agree.
 *
 * Mutations run on one thread (the engine thread); the UI only reads [ui].
 */
object Engine : CameraEvents, LinkEvents {
    private const val TAG = "RelyCam"
    private const val GIVE_UP_CAMERA_MS = 60_000L

    private lateinit var app: Context
    lateinit var prefs: Prefs; private set
    private lateinit var camera: CameraController
    private lateinit var orientation: DeviceOrientation
    private val thread = HandlerThread("relyacam-engine").apply { start() }
    private val h = Handler(thread.looper)
    private val qrThread = HandlerThread("relyacam-qr").apply { start() }
    private val qrHandler = Handler(qrThread.looper)

    private val _ui = MutableStateFlow(UiState())
    val ui: StateFlow<UiState> = _ui

    /** Kept apart from [ui] because it changes 20 times a second: only the meters redraw. */
    private val _level = MutableStateFlow(MicLevel())
    val level: StateFlow<MicLevel> = _level
    private var clipUntil = 0L

    /** The phone's roll for the level line (see [Tilt]); only measured while the line shows. */
    private val _tilt = MutableStateFlow<Float?>(null)
    val tilt: StateFlow<Float?> = _tilt
    private lateinit var tiltSensor: Tilt
    /** A snapshot asked for on the phone (so the phone says when it's sent), not from the PC. */
    private var snapFromPhone = false

    private lateinit var audio: AudioManager
    private var micSources: List<MicSource> = emptyList()
    private var bluetoothRouted = false

    private var visible = false
    private var preview: SurfaceTexture? = null
    private var previewSurface: Surface? = null
    private var reader: ImageReader? = null
    private var encoderCodecs: List<String> = emptyList()

    private var link: Link? = null
    private var pairing: Pairing? = null
    private var welcome: Welcome? = null
    private var venc: VideoEncoder? = null
    private var aenc: AudioEncoder? = null
    private var live = false
    private var attempt = 0
    private var droppedAt = 0L
    private val zooms = mutableMapOf<String, Double>()
    private var lastStateAt = 0L
    private var lastBytes = 0L

    fun init(context: Context) {
        if (::app.isInitialized) return
        app = context.applicationContext
        prefs = Prefs(app)
        camera = CameraController(app, this)
        orientation = DeviceOrientation(app) { post { onDeviceTurned() } }
        tiltSensor = Tilt(app) { _tilt.value = it }
        audio = app.getSystemService(AudioManager::class.java)
        _ui.update { it.copy(recent = prefs.pcs(), levelLine = prefs.levelLine) }
        // Called at once with what's connected now, then on every plug-in and unplug.
        audio.registerAudioDeviceCallback(object : AudioDeviceCallback() {
            override fun onAudioDevicesAdded(added: Array<out AudioDeviceInfo>) = post { refreshMics() }
            override fun onAudioDevicesRemoved(removed: Array<out AudioDeviceInfo>) = post { refreshMics() }
        }, h)
    }

    private fun post(block: () -> Unit) { h.post { runCatching(block).onFailure { Log.e(TAG, "engine", it) } } }
    private fun set(f: (UiState) -> UiState) = _ui.update(f)
    private val s get() = _ui.value

    // ---- permissions and visibility -------------------------------------------------------

    fun onPermissions(cameraOk: Boolean, micOk: Boolean) = post {
        val first = !s.cameraAllowed && cameraOk
        set { it.copy(cameraAllowed = cameraOk, micAllowed = micOk) }
        if (first) {
            val cams = CameraCatalog.discover(app)
            encoderCodecs = EncoderSupport().codecs()
            Log.i(TAG, "cameras: " + cams.joinToString { "${it.id}:${it.label} ${it.zoomMin}-${it.zoomMax} ${it.lenses} ${it.modes}" })
            set { it.copy(cams = cams, codecs = encoderCodecs) }
            val last = prefs.pcs().firstOrNull()
            if (last != null && prefs.wasLive) connectNow(last.pairing, quiet = true) else if (visible) startScan()
        }
    }

    fun onVisible(v: Boolean) = post {
        visible = v
        if (!v) {
            if (s.screen == Screen.SCAN || s.screen == Screen.GET_PLUGIN) camera.close()
        } else if (s.screen == Screen.SCAN) {
            startScan()
        }
        updateTilt()
    }

    /** The tilt sensor runs only while the level line is on screen. */
    private fun updateTilt() {
        if (visible && live && s.levelLine && !s.black && !s.micOnly) tiltSensor.start() else tiltSensor.stop()
    }

    // ---- scanning -------------------------------------------------------------------------

    /** Cameras that failed to stream this run, and modes a camera refused (see dropCamera). */
    private val broken = mutableSetOf<String>()
    private val failedModes = mutableMapOf<String, MutableSet<String>>()
    private var lastErrorCam: String? = null
    private var lastErrorAt = 0L

    private fun scanCam(): CameraInfo? = s.cams.firstOrNull { !it.front } ?: s.cams.firstOrNull()

    private fun startScan() {
        if (!s.cameraAllowed || !visible) return
        val cam = scanCam() ?: return
        val size = camera.previewSize(cam, 16, 9, 1920 * 1080)
        preview?.setDefaultBufferSize(size.width, size.height)
        reader?.close()
        val rs = camera.analysisSize(cam, 16, 9, 1280 * 720)
        val r = ImageReader.newInstance(rs.width, rs.height, ImageFormat.YUV_420_888, 2)
        r.setOnImageAvailableListener(QrReader, qrHandler)
        reader = r
        val z = 1.0.coerceIn(cam.zoomMin, cam.zoomMax)
        set { it.copy(screen = Screen.SCAN, cam = cam, zoom = z, torch = false, previewW = size.width, previewH = size.height) }
        camera.open(cam, Purpose.SCAN, Outputs(preview = previewSurface, reader = r.surface), 30, z)
    }

    private fun stopScan() {
        camera.closeAndWait()
        reader?.close()
        reader = null
    }

    fun showScan() = post {
        if (live) return@post
        attempt++
        set { it.copy(problem = null, reconnecting = false) }
        startScan()
    }

    fun showGetPlugin(on: Boolean) = post {
        if (live) return@post
        if (on) { camera.close(); set { it.copy(screen = Screen.GET_PLUGIN) } } else startScan()
    }

    /** One decoded frame at a time; frames that arrive while one is being read are skipped. */
    private object QrReader : ImageReader.OnImageAvailableListener {
        private var buf = ByteArray(0)
        private val scratch = Luma.Scratch()
        private var frame = 0L

        override fun onImageAvailable(r: ImageReader) {
            val img = runCatching { r.acquireLatestImage() }.getOrNull() ?: return
            val text = try {
                val plane = img.planes[0]
                val stride = plane.rowStride
                val need = stride * img.height
                if (buf.size < need) buf = ByteArray(need)
                val src = plane.buffer
                src.rewind()
                src.get(buf, 0, minOf(src.remaining(), need))
                QrDecode.fromLuma(buf, stride, img.width, img.height, frame++, scratch)
            } finally {
                img.close()
            }
            if (text != null) onQrText(text)
        }
    }

    fun onQrText(text: String) = post {
        if (s.screen != Screen.SCAN) return@post
        val p = Pairing.parse(text) ?: return@post
        connectNow(p, quiet = false)
    }

    /** A pairing link opened from outside (another scanner app, or the PC over USB). */
    fun onLink(uri: String) = post {
        val p = Pairing.parse(uri) ?: return@post
        if (live) {
            if (p.pcId == pairing?.pcId) return@post
            stopLive(sayBye = true)
        }
        connectNow(p, quiet = false)
    }

    fun connectByCode(code: String) = post {
        if (!Discovery.isCode(code)) { set { it.copy(notice = "A code is 6 letters and numbers, like K7Q-2MX.") }; return@post }
        set { it.copy(screen = Screen.CONNECTING, pcName = "code ${code.uppercase()}") }
        stopScan()
        val myAttempt = ++attempt
        Thread {
            val p = runCatching { Finder.byCode(code) }.getOrNull()
            post {
                if (myAttempt != attempt) return@post
                if (p == null) {
                    fail(null, ReachException(Reach.TIMEOUT, "No PC answered that code. Check the code, and that the Add phone panel is open in OBS."))
                } else connectNow(p, quiet = false)
            }
        }.start()
    }

    fun connectSaved(p: Pairing) = post { connectNow(p, quiet = false) }

    fun forget(pcId: String) = post {
        prefs.forget(pcId)
        set { it.copy(recent = prefs.pcs()) }
    }

    // ---- connecting -----------------------------------------------------------------------

    private fun hello() = Hello(
        secret = pairing?.secret.orEmpty(), device = prefs.deviceId, name = prefs.deviceName,
        app = BuildConfig.VERSION_NAME,
    )

    private fun connectNow(p: Pairing, quiet: Boolean) {
        stopScan()
        pairing = p
        val myAttempt = ++attempt
        set { it.copy(screen = Screen.CONNECTING, pcName = p.pcName.ifEmpty { p.hosts.first() }, problem = null) }
        Thread {
            var target = p
            var result = runCatching { Link.connect(target, hello(), this, ::needKeyFrame) }
            // A paired PC may have a new address: ask the network where it is now.
            if (result.isFailure && p.pcId.isNotEmpty() && (result.exceptionOrNull() as? ReachException)?.reach !in setOf(Reach.AUTH, Reach.VERSION)) {
                Finder.byId(p)?.let { fresh ->
                    if (fresh.hosts != p.hosts) {
                        target = fresh
                        result = runCatching { Link.connect(fresh, hello(), this, ::needKeyFrame) }
                    }
                }
            }
            post {
                if (myAttempt != attempt) { result.getOrNull()?.close(); return@post }
                result.fold(
                    onSuccess = { l -> pairing = target; onConnected(l) },
                    onFailure = { e ->
                        if (quiet) {
                            set { it.copy(notice = "Couldn't reach ${p.pcName.ifEmpty { "the PC" }}. Scan to connect.") }
                            prefs.wasLive = false
                            startScan()
                        } else fail(p, e)
                    },
                )
            }
        }.start()
    }

    private fun fail(p: Pairing?, e: Throwable) {
        val r = (e as? ReachException)?.reach
        val same = p?.let { Finder.sharesSubnet(it.hosts) } ?: true
        set { it.copy(screen = Screen.UNREACHABLE, problem = Problem(r, e.message ?: "Could not reach the PC.", p, same)) }
    }

    fun retry() = post {
        val p = s.problem?.pairing ?: pairing ?: return@post
        connectNow(p, quiet = false)
    }

    private fun onConnected(l: Link) {
        link = l
        welcome = l.welcome
        val p = pairing!!
        prefs.remember(p)
        set { it.copy(recent = prefs.pcs(), pcName = l.welcome.pc.ifEmpty { p.pcName }, transport = Finder.transportName(app, p.hosts.firstOrNull()), reconnecting = false, notice = null) }
        if (live) {
            // Back after a drop: the phone's state is the freshest, so it is sent, not replaced.
            sendCaps()
            sendVideoConfig()
            sendState(force = true)
            venc?.requestKeyFrame()
            return
        }
        startLive(l.welcome.restore)
    }

    // ---- live -----------------------------------------------------------------------------

    private fun startLive(restore: Cmd) {
        val cams = s.cams
        if (cams.isEmpty()) { fail(pairing, ReachException(Reach.OTHER, "This phone reported no usable camera.")); return }
        zooms.clear(); zooms.putAll(restore.zooms)
        val cam = cams.firstOrNull { it.id == restore.cam } ?: cams.firstOrNull { !it.front } ?: cams.first()
        val mode = pickMode(cam, restore.mode)
        val codec = pickCodec(restore.codec, mode)
        val zoom = (restore.zoom ?: zooms[cam.id] ?: 1.0).coerceIn(cam.zoomMin, cam.zoomMax)
        val micOnly = restore.micOnly ?: false
        live = true
        prefs.wasLive = true
        set {
            it.copy(
                screen = Screen.LIVE, cam = cam, mode = mode, codec = codec, zoom = zoom, torch = false,
                mirror = restore.mirror ?: false, flip = restore.flip ?: false, lock = restore.lock ?: false,
                mic = (restore.mic ?: true) || micOnly, black = false, micOnly = micOnly,
                micIn = restore.micIn?.takeIf { id -> it.mics.any { m -> m.id == id } } ?: it.micIn,
                ns = (restore.ns ?: false) && "ns" in it.clean, aec = (restore.aec ?: false) && "aec" in it.clean,
                agc = (restore.agc ?: false) && "agc" in it.clean,
                // The lock belongs to a point in one scene, so it's never restored; the rest is.
                aeLock = false, evIndex = evIndexFor(cam, restore.ev ?: 0.0), wb = wbFor(cam, restore.wb),
                grid = Grid().with(restore.grid), snap = "",
            )
        }
        LiveService.start(app, s.pcName, s.micAllowed)
        orientation.start()
        set { it.copy(rot = Upright.streamRotation(cam, orientation.degrees)) }
        sendCaps()
        openCamera()
        if (s.mic && s.micAllowed) startAudio()
        sendState(force = true)
        h.removeCallbacks(tick)
        h.postDelayed(tick, 1000)
        updateTilt()
    }

    /** [ev] (in EV) as the nearest of [cam]'s own steps, inside its range. */
    private fun evIndexFor(cam: CameraInfo, ev: Double): Int =
        if (!cam.hasEv) 0 else Math.round(ev / cam.evStep).toInt().coerceIn(cam.evMin, cam.evMax)

    /** [wanted] when [cam] has it, else auto. */
    private fun wbFor(cam: CameraInfo, wanted: String?): String =
        wanted?.takeIf { it in cam.whiteBalances } ?: WhiteBalance.AUTO

    /** Where the live camera draws; a snapshot can be up to the video's own size or bigger. */
    private fun liveOutputs(encoder: Surface? = venc?.surface): Outputs {
        val m = Mode.parse(s.mode)
        return Outputs(preview = previewSurface, encoder = encoder, stillsUpTo = m?.let { Size(it.w, it.h) })
    }

    private fun pickMode(cam: CameraInfo, wanted: String?): String {
        val modes = cam.modes.map { it.toString() }
        return wanted?.takeIf { it in modes } ?: Mode.default(modes) ?: modes.first()
    }

    private fun pickCodec(wanted: String?, mode: String): String {
        val m = Mode.parse(mode)
        val pc = welcome?.codecs.orEmpty()
        val ok = { c: String -> c in encoderCodecs && (pc.isEmpty() || c in pc) && (m == null || EncoderSupport().codecSupports(c, m.w, m.h, m.fps)) }
        return wanted?.takeIf(ok) ?: "h264".takeIf(ok) ?: encoderCodecs.firstOrNull(ok) ?: "h264"
    }

    private fun openCamera() {
        val cam = s.cam ?: return
        val m = Mode.parse(s.mode) ?: return
        camera.closeAndWait()
        venc?.stop()
        venc = null
        if (s.micOnly) return
        val enc = try {
            VideoEncoder(s.codec, m.w, m.h, m.fps) { pts, flags, data -> link?.sendVideo(pts, flags, data) }
        } catch (e: Exception) {
            Log.e(TAG, "encoder", e)
            set { it.copy(notice = "This phone's encoder refused ${m.label}. Pick another quality.") }
            return
        }
        venc = enc
        sendVideoConfig()
        val size = camera.previewSize(cam, m.w, m.h, minOf(m.w * m.h, 1920 * 1080))
        preview?.setDefaultBufferSize(size.width, size.height)
        // A fresh camera starts unlocked; the snapshot size arrives with the session (onStillSize).
        set { it.copy(previewW = size.width, previewH = size.height, aeLock = false, snap = "") }
        camera.open(cam, Purpose.LIVE, liveOutputs(enc.surface), m.fps, s.zoom)
        camera.setPicture(s.evIndex, s.wb)
        if (s.torch) camera.setTorch(true)
    }

    private fun startAudio() {
        if (aenc != null) return
        val cam = s.cam ?: return
        val input = inputDevice()
        routeBluetooth(input)
        aenc = runCatching {
            AudioEncoder(cam.realtimeClock, input, fx(), { pts, flags, data -> link?.sendAudio(pts, flags, data) }, ::onLevel)
        }.onFailure { set { s -> s.copy(notice = "The microphone is busy in another app.") } }.getOrNull()
        if (aenc != null) link?.sendAudioConfig()
    }

    private fun stopAudio() {
        aenc?.stop()
        aenc = null
        routeBluetooth(null)
        _level.value = MicLevel()
    }

    private fun fx() = Fx(ns = s.ns, aec = s.aec, agc = s.agc)

    private fun inputDevice(): AudioDeviceInfo? = micSources.firstOrNull { it.id == s.micIn }?.device

    /** From the audio thread. */
    private fun onLevel(peak: Float, clipped: Boolean) {
        val now = SystemClock.elapsedRealtime()
        if (clipped) clipUntil = now + 1000
        _level.value = MicLevel(peak, now < clipUntil)
    }

    /**
     * The phone's inputs changed (or this is the first look). A mic that was unplugged falls back
     * to the first built-in one; a new one is listed but never switched to by itself.
     */
    private fun refreshMics() {
        micSources = Mics.list(audio)
        val list = micSources.map { MicInput(it.id, it.label) }
        val cur = s.micIn.takeIf { id -> list.any { it.id == id } } ?: list.firstOrNull()?.id.orEmpty()
        val moved = cur != s.micIn
        if (list != s.mics) Log.i(TAG, "mics: " + list.joinToString { "${it.id}=${it.label}" })
        set { it.copy(mics = list, micIn = cur, clean = Mics.effects()) }
        if (moved && aenc != null) {
            routeBluetooth(inputDevice())
            aenc?.setInput(inputDevice())
        }
        if (live) { sendCaps(); sendState(force = true) }
    }

    /**
     * A Bluetooth mic only records while the phone holds a call-style link to it, so that link is
     * opened for a Bluetooth input and closed for anything else. Built, not tested on hardware.
     */
    @Suppress("DEPRECATION")
    private fun routeBluetooth(input: AudioDeviceInfo?) {
        val bt = micSources.firstOrNull { it.device == input }?.bluetooth == true
        runCatching {
            if (Build.VERSION.SDK_INT >= 31) {
                if (bt) {
                    val out = audio.availableCommunicationDevices
                    (out.firstOrNull { it.type == input!!.type } ?: out.firstOrNull { it.type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO })
                        ?.let { bluetoothRouted = audio.setCommunicationDevice(it) }
                } else if (bluetoothRouted) {
                    audio.clearCommunicationDevice()
                    bluetoothRouted = false
                }
            } else if (bt && !bluetoothRouted) {
                audio.startBluetoothSco()
                audio.isBluetoothScoOn = true
                bluetoothRouted = true
            } else if (!bt && bluetoothRouted) {
                audio.stopBluetoothSco()
                audio.isBluetoothScoOn = false
                bluetoothRouted = false
            }
        }.onFailure { Log.w(TAG, "bluetooth mic", it) }
    }

    private fun stopLive(sayBye: Boolean) {
        h.removeCallbacks(tick)
        h.removeCallbacks(reconnect)
        live = false
        orientation.stop()
        tiltSensor.stop()
        link?.close(sayBye)
        link = null
        camera.closeAndWait()
        venc?.stop(); venc = null
        stopAudio()
        LiveService.stop(app)
        set { it.copy(torch = false, black = false, reconnecting = false, micOnly = false, aeLock = false, snap = "") }
    }

    /** The Stop button: everything is released and the notification goes (must-never list). */
    fun stop() = post {
        attempt++
        prefs.wasLive = false
        stopLive(sayBye = true)
        startScan()
    }

    /** Cancel on the connecting and waiting screens. */
    fun cancel() = post {
        attempt++
        prefs.wasLive = false
        if (live) stopLive(sayBye = true)
        h.removeCallbacks(reconnect)
        set { it.copy(problem = null) }
        startScan()
    }

    // ---- requests: the PC's and the phone's go through here ------------------------------

    fun apply(c: Cmd, fromPc: Boolean = false) = post { applyNow(c, fromPc) }

    private fun applyNow(c: Cmd, fromPc: Boolean) {
        if (!live) {
            // On the scan screen only zoom and torch mean anything.
            c.zoom?.let { z -> s.cam?.let { cam -> val v = z.coerceIn(cam.zoomMin, cam.zoomMax); camera.setZoom(v, glide = false); set { it.copy(zoom = v) } } }
            c.torch?.let { t -> camera.setTorch(t); set { it.copy(torch = t && it.cam?.flash == true) } }
            return
        }
        var restart = false
        zooms.putAll(c.zooms)
        var cam = s.cam ?: return
        c.cam?.let { id ->
            val next = s.cams.firstOrNull { it.id == id }
            if (next != null && next.id != cam.id) {
                zooms[cam.id] = s.zoom
                cam = next
                restart = true
            }
        }
        var mode = s.mode
        var codec = s.codec
        if (restart && mode !in cam.modes.map { it.toString() }) mode = pickMode(cam, null)
        c.mode?.let { if (it in cam.modes.map { m -> m.toString() } && it != mode) { mode = it; restart = true } }
        c.codec?.let { if (it != codec && pickCodec(it, mode) == it) { codec = it; restart = true } }
        if (restart && codec != pickCodec(codec, mode)) codec = pickCodec(codec, mode)

        var zoom = s.zoom
        if (restart && c.cam != null) zoom = (zooms[cam.id] ?: 1.0)
        c.zoom?.let { zoom = it }
        zoom = zoom.coerceIn(cam.zoomMin, cam.zoomMax)
        zooms[cam.id] = zoom

        val wasMicOnly = s.micOnly
        val micOnly = c.micOnly ?: wasMicOnly
        val torch = (c.torch ?: s.torch) && cam.flash && !micOnly
        val lock = c.lock ?: s.lock
        val rot = if (lock) s.rot else Upright.streamRotation(cam, orientation.degrees)
        val oldIn = s.micIn
        // Mic only with the mic muted would send nothing at all, so turning it on unmutes.
        val mic = (c.mic ?: s.mic) || (micOnly && !wasMicOnly)
        // Brightness is kept in EV across cameras (each has its own step); white balance falls
        // back to auto on a camera without that choice.
        val evIndex = c.ev?.let { evIndexFor(cam, it) } ?: if (restart) evIndexFor(cam, s.ev) else s.evIndex
        val wb = wbFor(cam, c.wb ?: s.wb)
        val oldLock = s.aeLock
        val aeLock = !restart && !micOnly && cam.aeLock && (c.aeLock ?: oldLock)
        set {
            it.copy(cam = cam, mode = mode, codec = codec, zoom = zoom, torch = torch, rot = rot, lock = lock,
                mirror = c.mirror ?: it.mirror, flip = c.flip ?: it.flip, mic = mic, micOnly = micOnly,
                micIn = c.micIn?.takeIf { id -> it.mics.any { m -> m.id == id } } ?: it.micIn,
                ns = (c.ns ?: it.ns) && "ns" in it.clean, aec = (c.aec ?: it.aec) && "aec" in it.clean,
                agc = (c.agc ?: it.agc) && "agc" in it.clean,
                evIndex = evIndex, wb = wb, aeLock = aeLock, grid = it.grid.with(c.grid))
        }
        if (micOnly && !wasMicOnly) {
            camera.closeAndWait()
            venc?.stop()
            venc = null
            if (restart) sendCaps()
        } else if (restart || (wasMicOnly && !micOnly)) {
            openCamera()
            if (restart) sendCaps()
        } else if (!micOnly) {
            if (c.zoom != null) camera.setZoom(zoom, glide = fromPc)
            if (c.torch != null) camera.setTorch(torch)
            if (c.ev != null || c.wb != null) camera.setPicture(evIndex, wb)
            // The PC has no point to tap, so its lock meters the middle of the picture.
            if (aeLock && !oldLock) camera.focusAt(0.5f, 0.5f, lock = true)
            else if (!aeLock && oldLock) camera.unlock()
            if (c.snap) takeSnap(fromPhone = !fromPc)
        }
        if (s.mic && s.micAllowed) startAudio() else stopAudio()
        updateTilt()
        if (s.micIn != oldIn) {
            routeBluetooth(inputDevice())
            aenc?.setInput(inputDevice())
        }
        aenc?.setFx(fx())
        if (c.keyframe) venc?.requestKeyFrame()
        sendState(force = true)
    }

    /** A pinch on the phone: the camera follows the fingers with no glide. */
    fun pinch(factor: Float) = post {
        val cam = s.cam ?: return@post
        val z = (camera.zoom * factor).coerceIn(cam.zoomMin, cam.zoomMax)
        if (live) applyNow(Cmd(zoom = z), fromPc = false) else { camera.setZoom(z, glide = false); set { it.copy(zoom = z) } }
    }

    /**
     * A tap on the picture, already mapped to the sensor's own frame (0..1) by the view. A plain
     * tap focuses there for a while (and clears a lock, as camera apps do); [lock] (a long press)
     * locks focus and exposure there.
     */
    fun focusSensor(sx: Float, sy: Float, lock: Boolean = false) = post {
        val cam = s.cam ?: return@post
        val locked = lock && cam.aeLock && live
        camera.focusAt(sx, sy, locked)
        if (live && locked != s.aeLock) { set { it.copy(aeLock = locked) }; sendState(force = true) }
    }

    private fun takeSnap(fromPhone: Boolean) {
        if (s.snap.isEmpty()) return
        snapFromPhone = fromPhone
        camera.takeStill(s.rot)
    }

    fun setLevelLine(on: Boolean) = post {
        prefs.levelLine = on
        set { it.copy(levelLine = on) }
        updateTilt()
    }

    fun setBlack(on: Boolean) = post {
        set { it.copy(black = on && live) }
        updateTilt()
    }
    fun clearNotice() = set { it.copy(notice = null) }
    /** A short line at the bottom (an icon button's name on a long press). */
    fun hint(text: String) = set { it.copy(notice = text) }
    fun setUpdate(r: Release?) = set { it.copy(update = r) }

    // ---- preview surface ------------------------------------------------------------------

    fun attachPreview(st: SurfaceTexture) {
        val done = CountDownLatch(1)
        post {
            preview = st
            previewSurface = Surface(st)
            set { it.copy() }
            st.setDefaultBufferSize(s.previewW, s.previewH)
            if (live) {
                camera.setOutputs(liveOutputs())
            } else if (s.screen == Screen.SCAN) {
                startScan()
            }
            done.countDown()
        }
        done.await(1500, TimeUnit.MILLISECONDS)
    }

    /** Called before the view's surface goes away; waits until the camera no longer draws into it. */
    fun detachPreview() {
        val done = CountDownLatch(1)
        post {
            val old = previewSurface
            preview = null
            previewSurface = null
            if (live) camera.setOutputs(liveOutputs())
            else if (camera.info != null) camera.setOutputs(Outputs(reader = reader?.surface))
            old?.release()
            done.countDown()
        }
        done.await(1500, TimeUnit.MILLISECONDS)
    }

    // ---- camera and link events -----------------------------------------------------------

    override fun onZoom(zoom: Double) {
        set { it.copy(zoom = zoom) }
        if (live) post { sendState(force = false) }
    }

    override fun onStillSize(size: Size?) = post {
        val snap = size?.let { "${it.width}x${it.height}" }.orEmpty()
        if (snap == s.snap) return@post
        set { it.copy(snap = snap) }
        if (live) sendState(force = true)
    }

    override fun onStill(jpeg: ByteArray) = post {
        val fromPhone = snapFromPhone
        snapFromPhone = false
        val sent = link?.sendSnap(jpeg) == true
        Log.i(TAG, "snapshot ${jpeg.size} bytes, sent=$sent")
        if (!sent) set { it.copy(notice = "The photo couldn't be sent to the PC.") }
        else if (fromPhone) set { it.copy(notice = "Photo sent to ${it.pcName.ifEmpty { "the PC" }}.") }
    }

    override fun onError(info: CameraInfo, message: String) = post {
        Log.w(TAG, "camera ${info.id}: $message")
        if (!info.listed) {
            // Often transient (the scanner's session still closing as the live one opens), so the
            // same camera gets one more try. A second failure within 10 s drops it.
            val now = SystemClock.elapsedRealtime()
            val again = lastErrorCam == info.id && now - lastErrorAt < 10_000
            lastErrorCam = info.id
            lastErrorAt = now
            if (!again) {
                h.postDelayed({ runCatching { if (live) openCamera() else if (s.screen == Screen.SCAN) startScan() } }, 500)
                return@post
            }
            dropCamera(info, "${info.label} isn't usable on this phone; switched.")
            return@post
        }
        set { it.copy(notice = message) }
    }

    /**
     * The camera refused this quality. That's a fact about one mode, not about the camera: take
     * the mode off that camera's list and step down to the next one, and only when none is left
     * give up on the camera.
     */
    override fun onConfigFailed(info: CameraInfo) = post {
        Log.w(TAG, "camera ${info.id}: session refused ${s.mode}")
        val cam = s.cams.firstOrNull { it.id == info.id } ?: return@post
        if (!live) { dropCamera(cam, "${cam.label} can't run the scanner on this phone; switched."); return@post }
        val failed = Mode.parse(s.mode) ?: return@post
        failedModes.getOrPut(cam.id) { mutableSetOf() } += failed.toString()
        val left = cam.modes.filter { it.toString() != failed.toString() }
        if (left.isEmpty()) { dropCamera(cam, "${cam.label} won't stream on this phone; switched."); return@post }
        val trimmed = cam.copy(modes = left)
        val cost = { w: Int, h: Int, fps: Int -> w.toLong() * h * fps }
        val next = (left.filter { cost(it.w, it.h, it.fps) < cost(failed.w, failed.h, failed.fps) }
            .maxByOrNull { cost(it.w, it.h, it.fps) } ?: left.first()).toString()
        val label = Mode.parse(next)?.label ?: next
        set {
            it.copy(
                cams = it.cams.map { c -> if (c.id == cam.id) trimmed else c },
                cam = if (it.cam?.id == cam.id) trimmed else it.cam,
                mode = next, codec = pickCodec(it.codec, next),
                notice = "${cam.label} can't send ${failed.label} on this phone; using $label.",
            )
        }
        sendCaps()
        openCamera()
        sendState(force = true)
    }

    /**
     * Gives up on a camera for this run. Discovery runs again without it, so when it was a logical
     * camera its own lenses come back (dropping the nova 10's hidden camera 4 must still leave its
     * 1x back camera, not just the front one). The replacement faces the same way when it can.
     */
    private fun dropCamera(info: CameraInfo, notice: String) {
        broken += info.id
        val cams = CameraCatalog.discover(app, broken)
            .map { c -> c.copy(modes = c.modes.filter { it.toString() !in failedModes[c.id].orEmpty() }) }
            .filter { it.modes.isNotEmpty() }
        Log.i(TAG, "dropped camera ${info.id}; now: " + cams.joinToString { "${it.id}:${it.label}" })
        set { it.copy(cams = cams, notice = notice) }
        if (live) {
            val repl = cams.firstOrNull { it.facing == info.facing } ?: cams.firstOrNull { !it.front } ?: cams.firstOrNull()
            if (repl == null) { set { it.copy(notice = "No camera on this phone will stream.") }; return }
            applyNow(Cmd(cam = repl.id), false)
            sendCaps()
        } else {
            startScan()
        }
    }

    private fun needKeyFrame() { venc?.requestKeyFrame() }

    override fun onCmd(c: Cmd) = apply(c, fromPc = true)

    override fun onClosed(reason: String, byPeer: Boolean) = post {
        if (!live) return@post
        link = null
        if (byPeer) {
            // OBS closed or removed the source: release the camera now, and wait for it to come back.
            stopLive(sayBye = false)
            set { it.copy(screen = Screen.WAITING, problem = Problem(null, reason, pairing, true)) }
            h.postDelayed(reconnect, 3000)
        } else {
            droppedAt = System.currentTimeMillis()
            set { it.copy(reconnecting = true) }
            h.postDelayed(reconnect, 1000)
        }
    }

    /** Auto-reconnect: every few seconds, the same PC, at its last address or a fresh one. */
    private val reconnect = object : Runnable {
        override fun run() {
            val p = pairing ?: return
            val waiting = s.screen == Screen.WAITING
            if (!live && !waiting) return
            if (live && System.currentTimeMillis() - droppedAt > GIVE_UP_CAMERA_MS) {
                // A long outage: stop heating the phone; keep trying without the camera.
                stopLive(sayBye = false)
                set { it.copy(screen = Screen.WAITING, problem = Problem(null, "Lost the PC. Trying again every few seconds.", p, Finder.sharesSubnet(p.hosts))) }
            }
            val myAttempt = attempt
            Thread {
                var r = runCatching { Link.connect(p, hello(), this@Engine, ::needKeyFrame) }
                if (r.isFailure) Finder.byId(p)?.takeIf { it.hosts != p.hosts }?.let { f ->
                    r = runCatching { Link.connect(f, hello(), this@Engine, ::needKeyFrame) }
                    if (r.isSuccess) pairing = f
                }
                post {
                    if (myAttempt != attempt) { r.getOrNull()?.close(); return@post }
                    val l = r.getOrNull()
                    if (l != null) {
                        if (!live) { prefs.wasLive = true }
                        onConnected(l)
                    } else {
                        val e = r.exceptionOrNull() as? ReachException
                        if (e?.reach == Reach.AUTH) { stopLive(false); fail(p, e); return@post }
                        h.postDelayed(this, if (live) 2000 else 3000)
                    }
                }
            }.start()
        }
    }

    private fun onDeviceTurned() {
        if (!live || s.lock) return
        val cam = s.cam ?: return
        val rot = Upright.streamRotation(cam, orientation.degrees)
        if (rot != s.rot) { set { it.copy(rot = rot) }; sendState(force = true) }
    }

    // ---- outgoing -------------------------------------------------------------------------

    private fun caps(): Caps = Caps(
        device = prefs.deviceName,
        cams = s.cams.map {
            CamCaps(
                it.id, it.facingName, it.label, it.zoomMin, it.zoomMax, it.lenses, it.flash, it.modes.map { m -> m.toString() },
                ev = if (it.hasEv) listOf(it.evMin * it.evStep, it.evMax * it.evStep, it.evStep) else emptyList(),
                wb = it.whiteBalances, aeLock = it.aeLock,
            )
        },
        codecs = encoderCodecs,
        mic = s.micAllowed,
        mics = s.mics,
        clean = s.clean,
    )

    private fun sendCaps() { link?.sendCaps(caps()) }

    private fun sendVideoConfig() {
        val m = Mode.parse(s.mode) ?: return
        link?.sendVideoConfig(s.codec, m.w, m.h, m.fps)
    }

    private fun sendState(force: Boolean) {
        val now = System.currentTimeMillis()
        if (!force && now - lastStateAt < 33) {
            h.removeCallbacks(trailingState)
            h.postDelayed(trailingState, 40)
            return
        }
        lastStateAt = now
        val u = s
        link?.sendState(
            CamState(
                cam = u.cam?.id.orEmpty(), zoom = u.zoom, torch = u.torch, mic = u.mic && aenc != null,
                mode = u.mode, codec = u.codec, rot = u.rot, mirror = u.mirror, flip = u.flip,
                lock = u.lock, temp = u.temp, battery = u.battery, live = live && link != null,
                micIn = u.micIn, ns = u.ns, aec = u.aec, agc = u.agc, micOnly = u.micOnly,
                aeLock = u.aeLock, ev = u.ev, wb = u.wb, snap = if (u.micOnly) "" else u.snap, grid = u.grid,
            )
        )
    }

    private val trailingState = Runnable { sendState(force = true) }

    /** Once a second while live: battery, temperature and throughput for both screens. */
    private val tick = object : Runnable {
        override fun run() {
            if (!live) return
            val b = runCatching { app.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED)) }.getOrNull()
            val temp = (b?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) ?: 0) / 10.0
            val level = b?.let {
                val l = it.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
                val sc = it.getIntExtra(BatteryManager.EXTRA_SCALE, 100)
                if (l >= 0 && sc > 0) l * 100 / sc else -1
            } ?: -1
            val l = link
            val bytes = l?.sentBytes ?: 0L
            val kbps = if (bytes >= lastBytes) ((bytes - lastBytes) * 8 / 1000).toInt() else 0
            lastBytes = bytes
            val changed = temp != s.temp || level != s.battery
            set { it.copy(temp = temp, battery = level, kbps = kbps, rttMs = l?.rttMs ?: 0) }
            if (changed) sendState(force = true)
            h.postDelayed(this, 1000)
        }
    }

    fun micGranted() = ContextCompat.checkSelfPermission(app, android.Manifest.permission.RECORD_AUDIO) ==
        android.content.pm.PackageManager.PERMISSION_GRANTED
}
