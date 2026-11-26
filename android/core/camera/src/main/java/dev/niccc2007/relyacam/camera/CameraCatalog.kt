package dev.niccc2007.relyacam.camera

import android.content.Context
import android.graphics.ImageFormat
import android.graphics.Rect
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.util.Log
import android.util.Size
import kotlin.math.hypot
import kotlin.math.round

/** A recordable mode: a size the camera can output to an encoder at a steady frame rate. */
data class VideoMode(val w: Int, val h: Int, val fps: Int) {
    override fun toString() = "${w}x$h@$fps"
}

/** One camera as this app offers it. Every value is read from Camera2; nothing is per-phone. */
data class CameraInfo(
    val id: String,
    val facing: Int,
    val label: String,
    val sensorOrientation: Int,
    val zoomMin: Double,
    val zoomMax: Double,
    /** Zoom values where a physical lens takes over, inside [zoomMin, zoomMax]. */
    val lenses: List<Double>,
    val flash: Boolean,
    val modes: List<VideoMode>,
    /** True when zoom is CONTROL_ZOOM_RATIO (API 30+); false means a crop-region digital zoom. */
    val ratioZoom: Boolean,
    val activeArray: Rect,
    val afRegions: Boolean,
    val aeRegions: Boolean,
    val listed: Boolean,
    val physicalIds: Set<String>,
    /** CaptureRequest timestamps' clock: true = elapsedRealtimeNanos, false = nanoTime. */
    val realtimeClock: Boolean,
) {
    val front: Boolean get() = facing == CameraCharacteristics.LENS_FACING_FRONT
    val facingName: String
        get() = when (facing) {
            CameraCharacteristics.LENS_FACING_FRONT -> "front"
            CameraCharacteristics.LENS_FACING_BACK -> "back"
            else -> "external"
        }
}

/**
 * Finds the cameras worth offering.
 *
 * `getCameraIdList()` is not the whole story: some phones leave openable cameras out of it (the
 * owner's nova 10 lists [0, 1] while its logical camera 4 reaches the 0.65x ultrawide). So ids
 * past the list are tried too, and anything that answers is a candidate. Cameras are then hidden
 * by rule, never by id:
 *  - a physical member of an offered logical camera (the logical one already reaches its lens);
 *  - a sensor whose largest output is under 720p (depth, macro and helper sensors).
 */
object CameraCatalog {
    private const val TAG = "RelyCam"
    private const val PROBE_IDS = 16
    private const val MIN_PIXELS = 1280 * 720

    /** The 16:9 sizes a stream may use. Only the ones a camera and the encoder both support are offered. */
    private val SIZES = listOf(Size(1280, 720), Size(1920, 1080), Size(2560, 1440), Size(3840, 2160))
    private val RATES = listOf(30, 60)

    /**
     * Every usable camera. [exclude] leaves out cameras that failed to stream this run; the
     * physical lenses of an excluded logical camera are then offered again instead of hidden.
     */
    fun discover(context: Context, exclude: Set<String> = emptySet()): List<CameraInfo> {
        val mgr = context.getSystemService(CameraManager::class.java)
        val listed = runCatching { mgr.cameraIdList.toList() }.getOrDefault(emptyList())
        val ids = (listed + (0 until PROBE_IDS).map { it.toString() }).distinct().filter { it !in exclude }
        val enc = EncoderSupport()
        val read = ids.mapNotNull { id ->
            runCatching { read(mgr, id, id in listed, enc) }
                .onFailure { if (id in listed) Log.w(TAG, "camera $id unreadable", it) }
                .getOrNull()
        }
        val members = read.filter { it.physicalIds.isNotEmpty() }.flatMap { it.physicalIds }.toSet()
        val offered = read.filter { it.id !in members && it.modes.isNotEmpty() }
            .sortedWith(compareBy({ facingOrder(it.facing) }, { it.zoomMin }, { !it.listed }, { it.id.toIntOrNull() ?: 99 }))
        val counts = mutableMapOf<Int, Int>()
        return offered.map { c ->
            val n = (counts[c.facing] ?: 0) + 1
            counts[c.facing] = n
            val base = c.facingName.replaceFirstChar { it.uppercase() }
            c.copy(label = if (n == 1) base else "$base $n")
        }
    }

    private fun facingOrder(f: Int) = when (f) {
        CameraCharacteristics.LENS_FACING_BACK -> 0
        CameraCharacteristics.LENS_FACING_FRONT -> 1
        else -> 2
    }

    private fun read(mgr: CameraManager, id: String, listed: Boolean, enc: EncoderSupport): CameraInfo? {
        val c = mgr.getCameraCharacteristics(id)
        val map = c[CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP] ?: return null
        val caps = c[CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES] ?: IntArray(0)
        if (CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE !in caps) return null
        val yuv = map.getOutputSizes(ImageFormat.YUV_420_888).orEmpty()
        val largest = yuv.maxOfOrNull { it.width * it.height } ?: 0
        if (largest < MIN_PIXELS) return null

        val physical = if (Build.VERSION.SDK_INT >= 28 &&
            CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA in caps
        ) c.physicalCameraIds else emptySet()

        var ratioZoom = false
        var zMin = 1.0
        var zMax = (c[CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM] ?: 1f).toDouble()
        if (Build.VERSION.SDK_INT >= 30) {
            c[CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE]?.let {
                ratioZoom = true
                zMin = it.lower.toDouble()
                zMax = it.upper.toDouble()
            }
        }
        if (zMax < zMin) zMax = zMin

        val lenses = lensStops(mgr, c, physical, zMin, zMax)
        val modes = modes(c, map, enc)
        val active = c[CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE] ?: Rect(0, 0, 1, 1)
        val tsSource = c[CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE]
        return CameraInfo(
            id = id,
            facing = c[CameraCharacteristics.LENS_FACING] ?: CameraCharacteristics.LENS_FACING_EXTERNAL,
            label = "",
            sensorOrientation = c[CameraCharacteristics.SENSOR_ORIENTATION] ?: 0,
            zoomMin = zMin,
            zoomMax = zMax,
            lenses = lenses,
            flash = c[CameraCharacteristics.FLASH_INFO_AVAILABLE] == true,
            modes = modes,
            ratioZoom = ratioZoom,
            activeArray = active,
            afRegions = (c[CameraCharacteristics.CONTROL_MAX_REGIONS_AF] ?: 0) > 0,
            aeRegions = (c[CameraCharacteristics.CONTROL_MAX_REGIONS_AE] ?: 0) > 0,
            listed = listed,
            physicalIds = physical,
            realtimeClock = tsSource == CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME,
        )
    }

    /**
     * Where each lens takes over. The widest stop is the zoom range's own minimum when it is
     * below 1x (a logical camera reaches its ultrawide there). Longer lenses are worked out from
     * each physical camera's field of view against the logical camera's 1x: focal length over
     * sensor diagonal, which is what "35mm equivalent" measures.
     */
    private fun lensStops(
        mgr: CameraManager, c: CameraCharacteristics, physical: Set<String>, zMin: Double, zMax: Double,
    ): List<Double> {
        val stops = mutableListOf<Double>()
        if (zMin < 0.98) stops += zMin
        stops += if (1.0 in zMin..zMax) 1.0 else zMin
        val ref = fov(c)
        if (ref != null) {
            for (p in physical) {
                val pc = runCatching { mgr.getCameraCharacteristics(p) }.getOrNull() ?: continue
                val f = fov(pc) ?: continue
                val z = round(f / ref * 10) / 10.0
                if (z > 1.2 && z <= zMax + 1e-3) stops += z
            }
        }
        return stops.distinctBy { round(it * 100) }.sorted()
    }

    private fun fov(c: CameraCharacteristics): Double? {
        val f = c[CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS]?.firstOrNull() ?: return null
        val s = c[CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE] ?: return null
        val diag = hypot(s.width.toDouble(), s.height.toDouble())
        return if (diag > 0) f / diag else null
    }

    private fun modes(
        c: CameraCharacteristics, map: android.hardware.camera2.params.StreamConfigurationMap, enc: EncoderSupport,
    ): List<VideoMode> {
        val sizes = map.getOutputSizes(MediaCodec::class.java)?.toSet() ?: return emptyList()
        val ranges = c[CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES].orEmpty()
        val out = mutableListOf<VideoMode>()
        for (s in SIZES) {
            if (s !in sizes) continue
            val minNs = runCatching { map.getOutputMinFrameDuration(MediaCodec::class.java, s) }.getOrDefault(0L)
            for (fps in RATES) {
                if (ranges.none { it.upper == fps }) continue
                if (minNs > 0 && minNs > 1_000_000_000L / fps + 1000) continue
                if (!enc.supports(s.width, s.height, fps)) continue
                out += VideoMode(s.width, s.height, fps)
            }
        }
        return out
    }
}

/** Whether the phone has a hardware-friendly encoder for a size and rate (H.264 or HEVC). */
class EncoderSupport {
    private val infos = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.filter { it.isEncoder }

    private fun caps(mime: String): List<MediaCodecInfo.VideoCapabilities> = infos.mapNotNull { info ->
        if (info.supportedTypes.none { it.equals(mime, true) }) null
        else runCatching { info.getCapabilitiesForType(mime).videoCapabilities }.getOrNull()
    }

    private val avc by lazy { caps(MediaFormat.MIMETYPE_VIDEO_AVC) }
    private val hevc by lazy { caps(MediaFormat.MIMETYPE_VIDEO_HEVC) }

    fun supports(w: Int, h: Int, fps: Int): Boolean =
        (avc + hevc).any { runCatching { it.areSizeAndRateSupported(w, h, fps.toDouble()) }.getOrDefault(false) }

    fun codecs(): List<String> = buildList {
        if (avc.isNotEmpty()) add("h264")
        if (hevc.isNotEmpty()) add("hevc")
    }

    fun codecSupports(codec: String, w: Int, h: Int, fps: Int): Boolean =
        (if (codec == "hevc") hevc else avc).any {
            runCatching { it.areSizeAndRateSupported(w, h, fps.toDouble()) }.getOrDefault(false)
        }
}
