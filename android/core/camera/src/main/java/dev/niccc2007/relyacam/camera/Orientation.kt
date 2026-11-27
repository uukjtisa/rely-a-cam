package dev.niccc2007.relyacam.camera

import android.content.Context
import android.graphics.Matrix
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.view.OrientationEventListener
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.sqrt

/**
 * Which way up the phone is, as 0, 90, 180 or 270 (OrientationEventListener's convention: 90 =
 * left side up). Changes only once the phone is well past a diagonal, so holding it at 45 degrees
 * does not flicker between two answers.
 */
class DeviceOrientation(context: Context, private val onChange: (Int) -> Unit) {
    @Volatile var degrees = 0; private set

    private val listener = object : OrientationEventListener(context) {
        override fun onOrientationChanged(raw: Int) {
            if (raw == ORIENTATION_UNKNOWN) return
            val d = ((raw - degrees + 540) % 360) - 180
            if (kotlin.math.abs(d) < HYSTERESIS) return
            val q = ((raw + 45) / 90 * 90) % 360
            if (q != degrees) {
                degrees = q
                onChange(q)
            }
        }
    }

    fun start() { if (listener.canDetectOrientation()) listener.enable() }
    fun stop() = listener.disable()

    companion object { private const val HYSTERESIS = 55 }
}

/**
 * The phone's roll, for the level line: the angle of "up" in the device's own frame, in degrees.
 * 0 held upright in portrait, 90 turned a quarter counter-clockwise (Surface.ROTATION_90), so the
 * screen's own tilt is this minus the display rotation. Null while the phone lies nearly flat,
 * where roll means nothing.
 */
class Tilt(context: Context, private val onTilt: (Float?) -> Unit) {
    private val sm = context.getSystemService(SensorManager::class.java)
    private val sensor = sm?.getDefaultSensor(Sensor.TYPE_GRAVITY) ?: sm?.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
    private val g = FloatArray(3)
    private var primed = false
    private var on = false

    private val listener = object : SensorEventListener {
        override fun onSensorChanged(e: SensorEvent) {
            // The gravity sensor is already filtered; a raw accelerometer gets a light low-pass.
            val a = if (e.sensor.type == Sensor.TYPE_GRAVITY || !primed) 1f else 0.15f
            for (i in 0..2) g[i] += a * (e.values[i] - g[i])
            primed = true
            val n = sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2])
            if (n < 1f || abs(g[2]) > 0.9f * n) { onTilt(null); return }
            onTilt(Math.toDegrees(atan2(g[0].toDouble(), g[1].toDouble())).toFloat())
        }
        override fun onAccuracyChanged(s: Sensor?, accuracy: Int) {}
    }

    val available: Boolean get() = sensor != null

    fun start() {
        if (on || sensor == null) return
        on = true
        primed = false
        sm.registerListener(listener, sensor, SensorManager.SENSOR_DELAY_UI)
    }

    fun stop() {
        if (!on) return
        on = false
        sm?.unregisterListener(listener)
        onTilt(null)
    }
}

/** The geometry that keeps the picture upright, shared by the phone preview and the stream. */
object Upright {

    /**
     * How far clockwise the PC must turn a frame, straight off this sensor, to show it upright
     * (the STATE `rot` value). The same formula Android documents for JPEG_ORIENTATION.
     */
    fun streamRotation(cam: CameraInfo, deviceDegrees: Int): Int {
        val d = if (cam.front) -deviceDegrees else deviceDegrees
        return ((cam.sensorOrientation + d) % 360 + 360) % 360
    }

    /**
     * Maps a tap at ([ux], [uy]) on the upright picture (0..1, after mirror and flip) back to the
     * sensor's own frame, for tap-to-focus.
     */
    fun toSensor(ux: Float, uy: Float, rot: Int, mirror: Boolean, flip: Boolean): Pair<Float, Float> {
        val x = if (mirror) 1 - ux else ux
        val y = if (flip) 1 - uy else uy
        return when (rot) {
            90 -> y to 1 - x
            180 -> 1 - x to 1 - y
            270 -> 1 - y to x
            else -> x to y
        }
    }

    /**
     * The TextureView transform that shows the camera's preview upright, filling the view, and
     * exactly as OBS shows it (so mirror and flip apply here too, and a front camera is not
     * selfie-mirrored unless Mirror is on).
     *
     * A camera preview arrives in a SurfaceTexture already turned for the phone's natural
     * orientation, and front cameras arrive mirrored. The view stretches that to its own size.
     * This undoes the stretch, turns it against the screen's rotation, un-mirrors a front camera,
     * applies mirror and flip, and scales it to cover the view.
     *
     * @param buffer the preview buffer size as the camera writes it (sensor orientation, landscape).
     * @param displayDegrees the screen's rotation from natural (Display.getRotation() x 90).
     */
    fun previewMatrix(
        viewW: Float, viewH: Float, bufferW: Int, bufferH: Int, cam: CameraInfo,
        displayDegrees: Int, mirror: Boolean, flip: Boolean, fill: Boolean = true,
    ): Matrix {
        val sideways = cam.sensorOrientation % 180 != 0
        val cw = if (sideways) bufferH.toFloat() else bufferW.toFloat()
        val ch = if (sideways) bufferW.toFloat() else bufferH.toFloat()
        val m = Matrix()
        m.postScale(cw / viewW, ch / viewH)
        m.postTranslate(-cw / 2, -ch / 2)
        // Turn for the screen first, then un-mirror a front camera across the SCREEN's vertical
        // axis. The other order mirrors across the sensor's axis, which is the screen's horizontal
        // axis when the phone is sideways, and leaves the picture upside down in landscape.
        m.postRotate(-displayDegrees.toFloat())
        if (cam.front) m.postScale(-1f, 1f)
        if (mirror) m.postScale(-1f, 1f)
        if (flip) m.postScale(1f, -1f)
        val turned = displayDegrees % 180 != 0
        val rw = if (turned) ch else cw
        val rh = if (turned) cw else ch
        val s = if (fill) maxOf(viewW / rw, viewH / rh) else minOf(viewW / rw, viewH / rh)
        m.postScale(s, s)
        m.postTranslate(viewW / 2, viewH / 2)
        return m
    }
}
