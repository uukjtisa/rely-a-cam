package dev.niccc2007.relyacam.camera

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Rect
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.MeteringRectangle
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.graphics.ImageFormat
import android.media.ImageReader
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.Surface
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.math.abs
import kotlin.math.exp
import kotlin.math.ln
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * Where the frames go. Any of them may be absent; with none, the camera idles closed.
 * [stillsUpTo] asks for snapshots: the controller adds its own JPEG output, as large as the camera
 * allows next to the others. A photo size the camera refuses is dropped, never the video's mode.
 */
data class Outputs(
    val preview: Surface? = null,
    val encoder: Surface? = null,
    val reader: Surface? = null,
    val stillsUpTo: Size? = null,
) {
    val all: List<Surface> get() = listOfNotNull(preview, encoder, reader)
}

/** Focus and exposure lock, brightness (in the camera's own EV steps) and white balance. */
data class Picture(val aeLock: Boolean = false, val evIndex: Int = 0, val wb: String = "auto")

enum class Purpose { SCAN, LIVE }

interface CameraEvents {
    /** The zoom the camera is at now (gliding values included). Called on the camera thread. */
    fun onZoom(zoom: Double) {}
    fun onOpened(info: CameraInfo) {}
    fun onError(info: CameraInfo, message: String) {}
    /** The camera opened but refused these outputs (size or rate): the camera itself is fine. */
    fun onConfigFailed(info: CameraInfo) {}
    /** The photo size a snapshot takes now, or null when this session has none. Camera thread. */
    fun onStillSize(size: Size?) {}
    /** A snapshot's JPEG, upright. Camera thread. */
    fun onStill(jpeg: ByteArray) {}
}

/**
 * The one Camera2 owner (AGENTS.md rule 4): opening, the capture session, zoom with glide, torch,
 * tap-to-focus. Every call is posted to the camera thread, so callers never block on Camera2.
 */
@SuppressLint("MissingPermission") // the app asks for CAMERA before it opens anything
class CameraController(context: Context, private val events: CameraEvents) {
    private val mgr = context.getSystemService(CameraManager::class.java)
    private val thread = HandlerThread("relyacam-camera").apply { start() }
    private val handler = Handler(thread.looper)

    @Volatile var info: CameraInfo? = null; private set
    private var device: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var outputs = Outputs()
    private var purpose = Purpose.LIVE
    private var fps = 30
    private var generation = 0

    private var zoomTarget = 1.0
    @Volatile var zoom = 1.0; private set
    private var torch = false
    private var focusRegion: MeteringRectangle? = null
    private var lastTick = 0L
    private var gliding = false

    private var picture = Picture()
    /** The AE lock is applied a moment after the lock gesture, so exposure meters the new point first. */
    private var aeLocked = false
    private var still: ImageReader? = null
    /** Sizes already refused by this camera next to these outputs; never tried again this run. */
    private val refusedStills = mutableSetOf<String>()

    /** Opens [cam] (closing whatever was open) and starts streaming to [out]. */
    fun open(cam: CameraInfo, purpose: Purpose, out: Outputs, fps: Int, zoom: Double) = handler.post {
        closeNow()
        this.info = cam
        this.purpose = purpose
        this.outputs = out
        this.fps = fps
        this.zoomTarget = clamp(cam, zoom)
        this.zoom = zoomTarget
        this.focusRegion = null
        // A new camera starts unlocked; brightness and white balance carry over (clamped in fill()).
        this.aeLocked = false
        this.picture = picture.copy(aeLock = false)
        if (!cam.flash) torch = false
        val gen = ++generation
        try {
            mgr.openCamera(cam.id, object : CameraDevice.StateCallback() {
                override fun onOpened(d: CameraDevice) {
                    if (gen != generation) { d.close(); return }
                    device = d
                    events.onOpened(cam)
                    startSession()
                }
                override fun onDisconnected(d: CameraDevice) {
                    d.close()
                    if (gen == generation) { device = null; session = null; events.onError(cam, "Another app took the camera.") }
                }
                override fun onError(d: CameraDevice, error: Int) {
                    d.close()
                    if (gen == generation) { device = null; session = null; events.onError(cam, errorText(error)) }
                }
            }, handler)
        } catch (e: Exception) {
            events.onError(cam, e.message ?: "The camera would not open.")
        }
    }

    /** Swaps where frames go, for example when the preview appears or goes away. Waits for the swap. */
    fun setOutputs(out: Outputs) {
        val done = CountDownLatch(1)
        handler.post {
            if (out != outputs) {
                outputs = out
                runCatching { session?.close() }
                session = null
                startSession()
            }
            done.countDown()
        }
        done.await(800, TimeUnit.MILLISECONDS)
    }

    /**
     * Asks for [value]. [glide] eases there over about a fifth of a second in log space (requests
     * from the PC); without it the camera jumps (a pinch under the finger is already smooth).
     */
    fun setZoom(value: Double, glide: Boolean) = handler.post {
        val cam = info ?: return@post
        zoomTarget = clamp(cam, value)
        if (!glide) {
            zoom = zoomTarget
            applyRepeating()
            events.onZoom(zoom)
        } else if (!gliding) {
            gliding = true
            lastTick = System.nanoTime()
            handler.post(glideTick)
        }
    }

    fun setTorch(on: Boolean) = handler.post {
        torch = on && info?.flash == true
        applyRepeating()
    }

    /**
     * Focuses and meters at ([sx], [sy]), 0..1 across the sensor's own (unrotated) frame of what
     * is currently visible. Without [lock] it goes back to continuous focus after a few seconds;
     * with it, focus stays and exposure locks there (the camera app's "AE/AF lock"), until
     * [unlock] or the next tap.
     */
    fun focusAt(sx: Float, sy: Float, lock: Boolean = false) = handler.post {
        val cam = info ?: return@post
        handler.removeCallbacks(releaseFocus)
        handler.removeCallbacks(engageAeLock)
        aeLocked = false
        picture = picture.copy(aeLock = lock && cam.aeLock)
        if (cam.afRegions || cam.aeRegions) {
            val area = visibleArea(cam)
            val half = max(area.width(), area.height()) / 16
            val cx = area.left + (sx.coerceIn(0f, 1f) * area.width()).roundToInt()
            val cy = area.top + (sy.coerceIn(0f, 1f) * area.height()).roundToInt()
            val r = Rect(
                max(cx - half, 0), max(cy - half, 0),
                min(cx + half, cam.activeArray.width() - 1), min(cy + half, cam.activeArray.height() - 1),
            )
            focusRegion = MeteringRectangle(r, MeteringRectangle.METERING_WEIGHT_MAX)
            session?.let { s ->
                request()?.let { b ->
                    b.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_START)
                    runCatching { s.capture(b.build(), null, handler) }
                }
            }
        }
        applyRepeating()
        if (lock && cam.aeLock) handler.postDelayed(engageAeLock, 450)
        else if (!lock) handler.postDelayed(releaseFocus, 5000)
    }

    /** Back to continuous focus and auto exposure. */
    fun unlock() = handler.post {
        handler.removeCallbacks(engageAeLock)
        picture = picture.copy(aeLock = false)
        aeLocked = false
        releaseFocus.run()
    }

    /** Brightness and white balance; the lock is set through [focusAt] and [unlock]. */
    fun setPicture(evIndex: Int, wb: String) = handler.post {
        val cam = info
        val ev = if (cam != null && cam.hasEv) evIndex.coerceIn(cam.evMin, cam.evMax) else 0
        if (ev == picture.evIndex && wb == picture.wb) return@post
        picture = picture.copy(evIndex = ev, wb = wb)
        applyRepeating()
    }

    /**
     * Takes one photo at the session's still size, turned upright by [rotation] (the same `rot`
     * the stream carries). The video keeps flowing: the request also feeds the encoder and preview.
     */
    fun takeStill(rotation: Int) = handler.post {
        val s = session ?: return@post
        val r = still ?: return@post
        val d = device ?: return@post
        val b = runCatching { d.createCaptureRequest(CameraDevice.TEMPLATE_VIDEO_SNAPSHOT) }.getOrNull() ?: return@post
        fill(b)
        outputs.all.forEach { b.addTarget(it) }
        b.addTarget(r.surface)
        b.set(CaptureRequest.JPEG_ORIENTATION, ((rotation % 360) + 360) % 360)
        b.set(CaptureRequest.JPEG_QUALITY, 92.toByte())
        runCatching { s.capture(b.build(), null, handler) }.onFailure { Log.w(TAG, "still", it) }
    }

    private val engageAeLock = Runnable {
        aeLocked = picture.aeLock
        applyRepeating()
    }

    fun close() = handler.post { closeNow(); info = null }

    /** Closes and waits, so a Surface the camera was drawing into can be released safely after. */
    fun closeAndWait() {
        val done = CountDownLatch(1)
        handler.post { closeNow(); info = null; done.countDown() }
        done.await(1500, TimeUnit.MILLISECONDS)
    }

    /** A YUV size for frame analysis (the QR reader), with the given aspect, at most [maxPixels]. */
    fun analysisSize(cam: CameraInfo, aspectW: Int, aspectH: Int, maxPixels: Int): Size {
        val map = runCatching {
            mgr.getCameraCharacteristics(cam.id)[CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP]
        }.getOrNull()
        val sizes = map?.getOutputSizes(android.graphics.ImageFormat.YUV_420_888).orEmpty()
        val same = sizes.filter { it.width * aspectH == it.height * aspectW && it.width * it.height <= maxPixels }
        return same.maxByOrNull { it.width * it.height }
            ?: sizes.filter { it.width * it.height <= maxPixels }.maxByOrNull { it.width * it.height }
            ?: Size(1280, 720)
    }

    fun release() {
        handler.post { closeNow() }
        thread.quitSafely()
    }

    private val releaseFocus = Runnable {
        focusRegion = null
        val s = session ?: return@Runnable
        val b = request() ?: return@Runnable
        b.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_CANCEL)
        runCatching { s.capture(b.build(), null, handler) }
        applyRepeating()
    }

    private val glideTick = object : Runnable {
        override fun run() {
            val now = System.nanoTime()
            val dt = (now - lastTick) / 1e9
            lastTick = now
            val a = ln(zoom)
            val b = ln(zoomTarget)
            val next = a + (b - a) * (1 - exp(-dt / GLIDE_TAU))
            zoom = if (abs(b - next) < 0.002) zoomTarget else exp(next)
            applyRepeating()
            events.onZoom(zoom)
            if (zoom != zoomTarget) handler.postDelayed(this, 16) else gliding = false
        }
    }

    private fun closeNow() {
        generation++
        handler.removeCallbacks(glideTick)
        handler.removeCallbacks(releaseFocus)
        handler.removeCallbacks(engageAeLock)
        gliding = false
        runCatching { session?.close() }
        runCatching { device?.close() }
        session = null
        device = null
        closeStill()
    }

    private fun closeStill() {
        val had = still != null
        runCatching { still?.close() }
        still = null
        if (had) events.onStillSize(null)
    }

    @Suppress("DEPRECATION")
    private fun startSession() {
        val d = device ?: return
        val surfaces = outputs.all
        if (surfaces.isEmpty()) return
        val gen = generation
        closeStill()
        val (reader, key) = pickStill(d, surfaces) ?: (null to "")
        // A photo size the camera refuses next to the video is dropped and the session tried again
        // without it, so a snapshot never costs the stream a quality mode.
        val refused = {
            Log.w(TAG, "session refused the still output $key; trying without it")
            refusedStills += key
            runCatching { reader?.close() }
            startSession()
        }
        try {
            d.createCaptureSession(surfaces + listOfNotNull(reader?.surface), object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(s: CameraCaptureSession) {
                    if (gen != generation || device !== d) { s.close(); runCatching { reader?.close() }; return }
                    session = s
                    still = reader
                    events.onStillSize(reader?.let { Size(it.width, it.height) })
                    applyRepeating()
                    events.onZoom(zoom)
                }
                override fun onConfigureFailed(s: CameraCaptureSession) {
                    if (gen != generation) return
                    if (reader != null) refused() else info?.let { events.onConfigFailed(it) }
                }
            }, handler)
        } catch (e: IllegalArgumentException) {
            Log.w(TAG, "session", e)
            if (reader != null) refused() else info?.let { events.onConfigFailed(it) }
        } catch (e: Exception) {
            Log.w(TAG, "session", e)
            info?.let { events.onError(it, e.message ?: "The camera session failed.") }
        }
    }

    /**
     * The JPEG output for snapshots, or null. Tries the full sensor first, then the largest photo
     * no bigger than the video ([Outputs.stillsUpTo]), which every LIMITED camera must allow next to
     * a preview and an encoder. LEGACY cameras get none (they can't snapshot while recording).
     */
    private fun pickStill(d: CameraDevice, surfaces: List<Surface>): Pair<ImageReader, String>? {
        val cam = info ?: return null
        val limit = outputs.stillsUpTo ?: return null
        if (purpose != Purpose.LIVE || cam.stills.isEmpty()) return null
        val level = runCatching { mgr.getCameraCharacteristics(cam.id)[CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL] }.getOrNull()
        if (level == null || level == CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY) return null
        val limitPx = limit.width.toLong() * limit.height
        val candidates = listOfNotNull(
            cam.stills.first(),
            cam.stills.firstOrNull { it.width.toLong() * it.height <= limitPx },
        ).distinct()
        for (size in candidates) {
            val key = "${cam.id}/${size.width}x${size.height}/${limit.width}x${limit.height}/${surfaces.size}"
            if (key in refusedStills) continue
            val r = ImageReader.newInstance(size.width, size.height, ImageFormat.JPEG, 2)
            if (!supported(d, surfaces + r.surface)) { refusedStills += key; r.close(); continue }
            r.setOnImageAvailableListener({ ir ->
                val img = runCatching { ir.acquireNextImage() }.getOrNull() ?: return@setOnImageAvailableListener
                try {
                    val buf = img.planes[0].buffer
                    val bytes = ByteArray(buf.remaining())
                    buf.get(bytes)
                    events.onStill(bytes)
                } finally {
                    img.close()
                }
            }, handler)
            return r to key
        }
        return null
    }

    /** Asks the camera first where Android can (API 31+); elsewhere the session itself is the test. */
    private fun supported(d: CameraDevice, surfaces: List<Surface>): Boolean {
        if (Build.VERSION.SDK_INT < 31) return true
        return try {
            val cfg = SessionConfiguration(
                SessionConfiguration.SESSION_REGULAR, surfaces.map { OutputConfiguration(it) },
                { it.run() }, object : CameraCaptureSession.StateCallback() {
                    override fun onConfigured(s: CameraCaptureSession) {}
                    override fun onConfigureFailed(s: CameraCaptureSession) {}
                },
            )
            d.isSessionConfigurationSupported(cfg)
        } catch (e: Exception) {
            true // the camera won't say: try it
        }
    }

    private fun request(): CaptureRequest.Builder? {
        val d = device ?: return null
        val template = if (outputs.encoder != null) CameraDevice.TEMPLATE_RECORD else CameraDevice.TEMPLATE_PREVIEW
        val b = runCatching { d.createCaptureRequest(template) }.getOrNull() ?: return null
        outputs.all.forEach { b.addTarget(it) }
        return if (fill(b)) b else null
    }

    /** Every setting this controller owns, written into [b]. False when no camera is open. */
    private fun fill(b: CaptureRequest.Builder): Boolean {
        val cam = info ?: return false
        val chars = runCatching { mgr.getCameraCharacteristics(cam.id) }.getOrNull()
        val afModes = chars?.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES) ?: IntArray(0)
        val region = focusRegion
        val wantAf = when {
            region != null && CaptureRequest.CONTROL_AF_MODE_AUTO in afModes -> CaptureRequest.CONTROL_AF_MODE_AUTO
            purpose == Purpose.LIVE && CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO in afModes -> CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO
            CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE in afModes -> CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE
            else -> null
        }
        wantAf?.let { b.set(CaptureRequest.CONTROL_AF_MODE, it) }
        b.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
        b.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
        b.set(CaptureRequest.FLASH_MODE, if (torch) CaptureRequest.FLASH_MODE_TORCH else CaptureRequest.FLASH_MODE_OFF)
        if (region != null) {
            if (cam.afRegions) b.set(CaptureRequest.CONTROL_AF_REGIONS, arrayOf(region))
            if (cam.aeRegions) b.set(CaptureRequest.CONTROL_AE_REGIONS, arrayOf(region))
        }
        fpsRange(chars)?.let { b.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, it) }
        if (cam.ratioZoom && Build.VERSION.SDK_INT >= 30) {
            b.set(CaptureRequest.CONTROL_ZOOM_RATIO, zoom.toFloat())
        } else {
            b.set(CaptureRequest.SCALER_CROP_REGION, cropFor(cam, zoom))
        }
        if (cam.aeLock) b.set(CaptureRequest.CONTROL_AE_LOCK, aeLocked)
        // Brightness still applies while exposure is locked (Camera2 adjusts a locked AE by it).
        if (cam.hasEv) b.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, picture.evIndex.coerceIn(cam.evMin, cam.evMax))
        val preset = CameraInfo.WB_MODES.firstOrNull { it.first == picture.wb }?.second
        when {
            preset != null && preset in cam.awbModes -> {
                b.set(CaptureRequest.CONTROL_AWB_MODE, preset)
                if (cam.awbLock) b.set(CaptureRequest.CONTROL_AWB_LOCK, false)
            }
            CaptureRequest.CONTROL_AWB_MODE_AUTO in cam.awbModes -> {
                b.set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_AUTO)
                if (cam.awbLock) b.set(CaptureRequest.CONTROL_AWB_LOCK, picture.wb == "lock")
            }
        }
        return true
    }

    private fun applyRepeating() {
        val s = session ?: return
        val b = request() ?: return
        try {
            s.setRepeatingRequest(b.build(), null, handler)
        } catch (e: Exception) {
            Log.w(TAG, "repeating", e)
        }
    }

    /** A steady rate for an encoder: [fps, fps] if offered, else the steadiest range that tops at fps. */
    private fun fpsRange(chars: CameraCharacteristics?): Range<Int>? {
        val ranges = chars?.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES) ?: return null
        val target = if (purpose == Purpose.SCAN) 30 else fps
        return ranges.filter { it.upper == target }.maxByOrNull { it.lower }
            ?: ranges.filter { it.upper <= target }.maxByOrNull { it.upper * 1000 + it.lower }
    }

    /** The part of the active array visible now, in the coordinates regions are given in. */
    private fun visibleArea(cam: CameraInfo): Rect =
        if (cam.ratioZoom && Build.VERSION.SDK_INT >= 30) Rect(0, 0, cam.activeArray.width(), cam.activeArray.height())
        else cropFor(cam, zoom)

    private fun cropFor(cam: CameraInfo, z: Double): Rect {
        val w = cam.activeArray.width()
        val h = cam.activeArray.height()
        val cw = (w / max(z, 1.0)).roundToInt()
        val ch = (h / max(z, 1.0)).roundToInt()
        val l = (w - cw) / 2
        val t = (h - ch) / 2
        return Rect(l, t, l + cw, t + ch)
    }

    private fun clamp(cam: CameraInfo, z: Double) = z.coerceIn(cam.zoomMin, cam.zoomMax)

    /** A size the camera can send to a preview surface, with the given aspect, at most [maxPixels]. */
    fun previewSize(cam: CameraInfo, aspectW: Int, aspectH: Int, maxPixels: Int): Size {
        val map = runCatching {
            mgr.getCameraCharacteristics(cam.id)[CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP]
        }.getOrNull()
        val sizes = map?.getOutputSizes(SurfaceTexture::class.java).orEmpty()
        val same = sizes.filter { it.width * aspectH == it.height * aspectW && it.width * it.height <= maxPixels }
        return same.maxByOrNull { it.width * it.height }
            ?: sizes.filter { it.width * it.height <= maxPixels }.maxByOrNull { it.width * it.height }
            ?: Size(1280, 720)
    }

    companion object {
        private const val TAG = "RelyCam"
        /** Glide time constant in seconds, matching the approved mock. */
        const val GLIDE_TAU = 0.07

        private fun errorText(code: Int) = when (code) {
            CameraDevice.StateCallback.ERROR_CAMERA_IN_USE -> "The camera is in use by another app."
            CameraDevice.StateCallback.ERROR_MAX_CAMERAS_IN_USE -> "Too many cameras are open. Close other camera apps."
            CameraDevice.StateCallback.ERROR_CAMERA_DISABLED -> "The camera is disabled by a device policy."
            CameraDevice.StateCallback.ERROR_CAMERA_DEVICE -> "The camera hit a fatal error. Try again."
            CameraDevice.StateCallback.ERROR_CAMERA_SERVICE -> "The camera service crashed. Restart the phone if this repeats."
            else -> "The camera failed (code $code)."
        }
    }
}
