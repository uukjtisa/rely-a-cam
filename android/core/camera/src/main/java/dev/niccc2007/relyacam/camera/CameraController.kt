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

/** Where the frames go. Any of them may be absent; with none, the camera idles closed. */
data class Outputs(val preview: Surface? = null, val encoder: Surface? = null, val reader: Surface? = null) {
    val all: List<Surface> get() = listOfNotNull(preview, encoder, reader)
}

enum class Purpose { SCAN, LIVE }

interface CameraEvents {
    /** The zoom the camera is at now (gliding values included). Called on the camera thread. */
    fun onZoom(zoom: Double) {}
    fun onOpened(info: CameraInfo) {}
    fun onError(info: CameraInfo, message: String) {}
    /** The camera opened but refused these outputs (size or rate): the camera itself is fine. */
    fun onConfigFailed(info: CameraInfo) {}
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
     * is currently visible. Goes back to continuous focus after a few seconds.
     */
    fun focusAt(sx: Float, sy: Float) = handler.post {
        val cam = info ?: return@post
        if (!cam.afRegions && !cam.aeRegions) return@post
        val area = visibleArea(cam)
        val half = max(area.width(), area.height()) / 16
        val cx = area.left + (sx.coerceIn(0f, 1f) * area.width()).roundToInt()
        val cy = area.top + (sy.coerceIn(0f, 1f) * area.height()).roundToInt()
        val r = Rect(
            max(cx - half, 0), max(cy - half, 0),
            min(cx + half, cam.activeArray.width() - 1), min(cy + half, cam.activeArray.height() - 1),
        )
        focusRegion = MeteringRectangle(r, MeteringRectangle.METERING_WEIGHT_MAX)
        val s = session ?: return@post
        val b = request() ?: return@post
        b.set(CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AF_TRIGGER_START)
        runCatching { s.capture(b.build(), null, handler) }
        applyRepeating()
        handler.removeCallbacks(releaseFocus)
        handler.postDelayed(releaseFocus, 5000)
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
        gliding = false
        runCatching { session?.close() }
        runCatching { device?.close() }
        session = null
        device = null
    }

    @Suppress("DEPRECATION")
    private fun startSession() {
        val d = device ?: return
        val surfaces = outputs.all
        if (surfaces.isEmpty()) return
        val gen = generation
        try {
            d.createCaptureSession(surfaces, object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(s: CameraCaptureSession) {
                    if (gen != generation || device !== d) { s.close(); return }
                    session = s
                    applyRepeating()
                    events.onZoom(zoom)
                }
                override fun onConfigureFailed(s: CameraCaptureSession) {
                    if (gen == generation) info?.let { events.onConfigFailed(it) }
                }
            }, handler)
        } catch (e: IllegalArgumentException) {
            Log.w(TAG, "session", e)
            info?.let { events.onConfigFailed(it) }
        } catch (e: Exception) {
            Log.w(TAG, "session", e)
            info?.let { events.onError(it, e.message ?: "The camera session failed.") }
        }
    }

    private fun request(): CaptureRequest.Builder? {
        val d = device ?: return null
        val cam = info ?: return null
        val template = if (outputs.encoder != null) CameraDevice.TEMPLATE_RECORD else CameraDevice.TEMPLATE_PREVIEW
        val b = runCatching { d.createCaptureRequest(template) }.getOrNull() ?: return null
        outputs.all.forEach { b.addTarget(it) }
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
        return b
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
