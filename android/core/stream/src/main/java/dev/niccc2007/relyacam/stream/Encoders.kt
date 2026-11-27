package dev.niccc2007.relyacam.stream

import android.annotation.SuppressLint
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaRecorder
import android.media.audiofx.AcousticEchoCanceler
import android.media.audiofx.AudioEffect
import android.media.audiofx.AutomaticGainControl
import android.media.audiofx.NoiseSuppressor
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.util.Log
import android.view.Surface
import dev.niccc2007.relyacam.protocol.FLAG_CONFIG
import dev.niccc2007.relyacam.protocol.FLAG_KEY

/** Encoded output: an Annex B access unit (video) or a raw AAC frame (audio). */
fun interface MediaSink {
    fun onMedia(ptsUs: Long, flags: Int, data: ByteArray)
}

/**
 * H.264 or HEVC from a Surface the camera draws into. Output is Annex B (start codes), which is
 * what Android encoders produce. Each key frame goes out with the codec config in front of it, so
 * the PC can start decoding at any key frame.
 */
class VideoEncoder(
    val codec: String,
    val width: Int,
    val height: Int,
    val fps: Int,
    private val sink: MediaSink,
) {
    private val mime = if (codec == "hevc") MediaFormat.MIMETYPE_VIDEO_HEVC else MediaFormat.MIMETYPE_VIDEO_AVC
    private val mc = MediaCodec.createEncoderByType(mime)
    val surface: Surface
    @Volatile private var running = true
    private var config: ByteArray = ByteArray(0)
    private val thread: Thread

    init {
        val f = MediaFormat.createVideoFormat(mime, width, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrate(width, height, fps, codec))
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            setInteger(MediaFormat.KEY_PRIORITY, 0)
            if (Build.VERSION.SDK_INT >= 29) setInteger(MediaFormat.KEY_PREPEND_HEADER_TO_SYNC_FRAMES, 1)
            if (Build.VERSION.SDK_INT >= 30) setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            // Repeat the last frame when the camera stalls, so a still scene still sends frames.
            setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 1_000_000L / fps * 3)
        }
        try {
            mc.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        } catch (e: Exception) {
            // Some encoders refuse low-latency or the prepend flag; the plain format always works.
            f.setInteger(MediaFormat.KEY_LOW_LATENCY, 0)
            mc.reset()
            mc.configure(MediaFormat.createVideoFormat(mime, width, height).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
                setInteger(MediaFormat.KEY_BIT_RATE, bitrate(width, height, fps, codec))
                setInteger(MediaFormat.KEY_FRAME_RATE, fps)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            }, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        }
        surface = mc.createInputSurface()
        mc.start()
        thread = Thread(::drain, "relyacam-venc").apply { start() }
    }

    fun requestKeyFrame() {
        if (!running) return
        runCatching { mc.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) }) }
    }

    private fun drain() {
        val info = MediaCodec.BufferInfo()
        while (running) {
            val i = try { mc.dequeueOutputBuffer(info, 100_000) } catch (e: IllegalStateException) { break }
            if (i < 0) continue
            val buf = mc.getOutputBuffer(i)
            if (buf != null && info.size > 0) {
                val bytes = ByteArray(info.size)
                buf.position(info.offset)
                buf.get(bytes)
                when {
                    info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0 -> {
                        config = bytes
                        sink.onMedia(info.presentationTimeUs, FLAG_CONFIG, bytes)
                    }
                    info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0 -> {
                        val withConfig = if (startsWith(bytes, config)) bytes else config + bytes
                        sink.onMedia(info.presentationTimeUs, FLAG_KEY, withConfig)
                    }
                    else -> sink.onMedia(info.presentationTimeUs, 0, bytes)
                }
            }
            runCatching { mc.releaseOutputBuffer(i, false) }
        }
    }

    fun stop() {
        running = false
        runCatching { thread.join(500) }
        runCatching { mc.stop() }
        runCatching { mc.release() }
        runCatching { surface.release() }
    }

    companion object {
        /** About 0.1 bit a pixel a frame for H.264; HEVC gets the same picture in about 70%. */
        fun bitrate(w: Int, h: Int, fps: Int, codec: String): Int {
            val b = (w.toLong() * h * fps / 10).coerceIn(2_000_000L, 40_000_000L)
            return (if (codec == "hevc") b * 7 / 10 else b).toInt()
        }

        private fun startsWith(a: ByteArray, prefix: ByteArray): Boolean {
            if (prefix.isEmpty() || a.size < prefix.size) return prefix.isEmpty()
            for (i in prefix.indices) if (a[i] != prefix[i]) return false
            return true
        }
    }
}

/** How loud the mic is: the peak of the last ~50 ms (0..1), and whether it reached full scale. */
fun interface LevelSink {
    fun onLevel(peak: Float, clipped: Boolean)
}

/**
 * The phone mic as AAC-LC, 48 kHz mono. Timestamps are on the same clock the camera stamps video
 * with ([realtimeClock]), so the PC could line them up.
 *
 * [mic] picks the microphone (null lets Android choose). The clean-up effects are attached to
 * this recording only, and only the ones the phone has; [setFx] turns them on and off live.
 */
@SuppressLint("MissingPermission") // the app asks for RECORD_AUDIO before it starts the mic
class AudioEncoder(
    private val realtimeClock: Boolean,
    mic: AudioDeviceInfo?,
    fx: Fx,
    private val sink: MediaSink,
    private val level: LevelSink = LevelSink { _, _ -> },
) {
    private val rate = 48_000
    private val record: AudioRecord
    private val mc = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC)
    @Volatile private var running = true
    @Volatile var muted = false
    private val input: Thread
    private val output: Thread
    private val ns: AudioEffect?
    private val aec: AudioEffect?
    private val agc: AudioEffect?

    init {
        val minBuf = AudioRecord.getMinBufferSize(rate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
        record = AudioRecord(MediaRecorder.AudioSource.CAMCORDER, rate, AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT, maxOf(minBuf, 8192) * 2)
        if (mic != null) record.preferredDevice = mic
        val session = record.audioSessionId
        ns = if (NoiseSuppressor.isAvailable()) runCatching { NoiseSuppressor.create(session) }.getOrNull() else null
        aec = if (AcousticEchoCanceler.isAvailable()) runCatching { AcousticEchoCanceler.create(session) }.getOrNull() else null
        agc = if (AutomaticGainControl.isAvailable()) runCatching { AutomaticGainControl.create(session) }.getOrNull() else null
        setFx(fx)
        val f = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_AAC, rate, 1).apply {
            setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC)
            setInteger(MediaFormat.KEY_BIT_RATE, 128_000)
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 16384)
        }
        mc.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        mc.start()
        record.startRecording()
        input = Thread(::feed, "relyacam-amic").apply { start() }
        output = Thread(::drain, "relyacam-aenc").apply { start() }
    }

    /** Switches the microphone while recording; Android re-routes without a restart. */
    fun setInput(d: AudioDeviceInfo?) {
        runCatching { record.preferredDevice = d }
    }

    fun setFx(fx: Fx) {
        runCatching { ns?.enabled = fx.ns }
        runCatching { aec?.enabled = fx.aec }
        runCatching { agc?.enabled = fx.agc }
    }

    private fun now(): Long = if (realtimeClock) SystemClock.elapsedRealtimeNanos() / 1000 else System.nanoTime() / 1000

    private fun feed() {
        val chunk = ByteArray(2048)
        var peak = 0
        var lastLevel = SystemClock.elapsedRealtime()
        while (running) {
            val n = record.read(chunk, 0, chunk.size)
            if (n <= 0) continue
            if (muted) chunk.fill(0, 0, n)
            var k = 0
            while (k + 1 < n) {
                val v = ((chunk[k].toInt() and 0xFF) or (chunk[k + 1].toInt() shl 8)).toShort().toInt()
                val a = if (v < 0) -v else v
                if (a > peak) peak = a
                k += 2
            }
            val t = SystemClock.elapsedRealtime()
            if (t - lastLevel >= 50) {
                level.onLevel(minOf(1f, peak / 32767f), peak >= CLIP)
                peak = 0
                lastLevel = t
            }
            val i = try { mc.dequeueInputBuffer(50_000) } catch (e: IllegalStateException) { break }
            if (i < 0) continue
            val buf = mc.getInputBuffer(i) ?: continue
            buf.clear()
            buf.put(chunk, 0, minOf(n, buf.remaining()))
            // The stamp of the first sample in this chunk: now, less what the chunk holds.
            val pts = now() - n / 2 * 1_000_000L / rate
            runCatching { mc.queueInputBuffer(i, 0, n, pts, 0) }
        }
    }

    private fun drain() {
        val info = MediaCodec.BufferInfo()
        while (running) {
            val i = try { mc.dequeueOutputBuffer(info, 100_000) } catch (e: IllegalStateException) { break }
            if (i < 0) continue
            val buf = mc.getOutputBuffer(i)
            if (buf != null && info.size > 0) {
                val bytes = ByteArray(info.size)
                buf.position(info.offset)
                buf.get(bytes)
                val flags = if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) FLAG_CONFIG else FLAG_KEY
                sink.onMedia(info.presentationTimeUs, flags, bytes)
            }
            runCatching { mc.releaseOutputBuffer(i, false) }
        }
    }

    fun stop() {
        running = false
        runCatching { record.stop() }
        runCatching { input.join(500) }
        runCatching { output.join(500) }
        for (e in listOf(ns, aec, agc)) runCatching { e?.release() }
        runCatching { record.release() }
        runCatching { mc.stop() }
        runCatching { mc.release() }
        Log.i("RelyCam", "mic released")
    }

    private companion object {
        /** A sample this close to full scale means the mic is clipping (about -0.2 dBFS). */
        const val CLIP = 32_000
    }
}
