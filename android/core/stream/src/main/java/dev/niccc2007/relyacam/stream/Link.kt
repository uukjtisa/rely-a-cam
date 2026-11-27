package dev.niccc2007.relyacam.stream

import android.util.Log
import dev.niccc2007.relyacam.protocol.Caps
import dev.niccc2007.relyacam.protocol.CamState
import dev.niccc2007.relyacam.protocol.Cmd
import dev.niccc2007.relyacam.protocol.FLAG_CONFIG
import dev.niccc2007.relyacam.protocol.FLAG_KEY
import dev.niccc2007.relyacam.protocol.Hello
import dev.niccc2007.relyacam.protocol.MAX_FRAME
import dev.niccc2007.relyacam.protocol.Pairing
import dev.niccc2007.relyacam.protocol.Type
import dev.niccc2007.relyacam.protocol.Welcome
import dev.niccc2007.relyacam.protocol.Wire
import org.json.JSONObject
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.DataInputStream
import java.io.IOException
import java.net.ConnectException
import java.net.InetSocketAddress
import java.net.NoRouteToHostException
import java.net.Socket
import java.net.SocketTimeoutException
import java.nio.ByteBuffer
import java.util.concurrent.ExecutorCompletionService
import java.util.concurrent.Executors
import java.util.concurrent.LinkedBlockingDeque
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicLong

/** Why a connection attempt failed, so the phone can say why instead of "failed" (rule 7). */
enum class Reach { REFUSED, TIMEOUT, NO_ROUTE, AUTH, VERSION, BUSY, OTHER }

class ReachException(val reach: Reach, message: String) : IOException(message)

interface LinkEvents {
    fun onWelcome(w: Welcome) {}
    fun onCmd(c: Cmd) {}
    /** The link is gone. [byPeer] is true when the PC said BYE (OBS closed or the source was removed). */
    fun onClosed(reason: String, byPeer: Boolean) {}
}

/**
 * One TCP connection to one PC. Two threads: a reader for commands and a
 * writer that owns the socket's output. Video never queues past [MAX_QUEUE_US]: past that, frames
 * are dropped up to the next key frame and the encoder is asked for one.
 */
class Link private constructor(
    private val socket: Socket,
    val welcome: Welcome,
    private val events: LinkEvents,
    private val onNeedKeyFrame: () -> Unit,
) {
    private class Out(val type: Int, val payload: ByteArray, val video: Boolean, val key: Boolean, val ptsUs: Long)

    private val queue = LinkedBlockingDeque<Out>()
    private val input = DataInputStream(BufferedInputStream(socket.getInputStream(), 64 * 1024))
    private val output = BufferedOutputStream(socket.getOutputStream(), 256 * 1024)
    @Volatile private var open = true
    @Volatile private var droppingToKey = false
    private val lastHeard = AtomicLong(System.currentTimeMillis())
    @Volatile var rttMs = 0L; private set
    @Volatile var sentBytes = 0L; private set

    private val writer = Thread(::writeLoop, "relyacam-tx")
    private val reader = Thread(::readLoop, "relyacam-rx")
    private val pinger = Thread(::pingLoop, "relyacam-ping")

    private fun startThreads() { writer.start(); reader.start(); pinger.start() }

    fun sendCaps(c: Caps) = control(Type.CAPS, c.toJson().toString())
    fun sendState(s: CamState) = control(Type.STATE, s.toJson().toString())
    fun sendVideoConfig(codec: String, w: Int, h: Int, fps: Int) =
        control(Type.VCONF, JSONObject().put("codec", codec).put("w", w).put("h", h).put("fps", fps).toString())
    fun sendAudioConfig() = control(Type.ACONF, JSONObject().put("codec", "aac").put("rate", 48000).put("ch", 1).toString())

    private fun control(type: Int, text: String) {
        if (!open) return
        // A newer STATE replaces a waiting one: the PC only needs the latest.
        if (type == Type.STATE) queue.removeIf { it.type == Type.STATE }
        queue.offerFirst(Out(type, text.toByteArray(Charsets.UTF_8), false, false, 0))
    }

    fun sendVideo(ptsUs: Long, flags: Int, data: ByteArray) {
        if (!open) return
        val key = flags and (FLAG_KEY or FLAG_CONFIG) != 0
        if (droppingToKey && !key) return
        if (key) droppingToKey = false
        val oldest = queue.firstOrNull { it.video }
        if (!key && oldest != null && ptsUs - oldest.ptsUs > MAX_QUEUE_US) {
            queue.removeIf { it.video }
            droppingToKey = true
            onNeedKeyFrame()
            return
        }
        queue.offerLast(Out(Type.VIDEO, Wire.media(ptsUs, flags, data), true, key, ptsUs))
    }

    fun sendAudio(ptsUs: Long, flags: Int, data: ByteArray) {
        if (!open) return
        if (queue.size > 200) return
        queue.offerLast(Out(Type.AUDIO, Wire.media(ptsUs, flags, data), false, false, ptsUs))
    }

    /**
     * A snapshot's JPEG (SNAP), queued behind what's waiting and never dropped for backpressure.
     * A few megabytes hold the video up for a moment, and the usual drop-to-key-frame absorbs it.
     * False when it's over the frame limit (the PC would drop the link) or the link is closed.
     */
    fun sendSnap(jpeg: ByteArray): Boolean {
        if (!open || jpeg.size > MAX_FRAME) return false
        queue.offerLast(Out(Type.SNAP, jpeg, false, false, 0))
        return true
    }

    /** Ends the link on purpose (BYE), releasing nothing else: the caller stops the camera. */
    fun close(sayBye: Boolean = true) {
        if (!open) return
        if (sayBye) runCatching {
            synchronized(output) { Wire.write(output, Type.BYE, ByteArray(0)); output.flush() }
        }
        shut("Stopped", byPeer = false, notify = false)
    }

    private fun shut(reason: String, byPeer: Boolean, notify: Boolean = true) {
        if (!open) return
        open = false
        queue.clear()
        queue.offer(Out(-1, ByteArray(0), false, false, 0))
        runCatching { socket.close() }
        if (notify) events.onClosed(reason, byPeer)
    }

    private fun writeLoop() {
        try {
            while (open) {
                val o = queue.poll(500, TimeUnit.MILLISECONDS) ?: continue
                if (o.type < 0) break
                synchronized(output) {
                    Wire.write(output, o.type, o.payload)
                    if (queue.isEmpty()) output.flush()
                }
                sentBytes += o.payload.size + 5
            }
        } catch (e: IOException) {
            shut("The connection dropped.", byPeer = false)
        }
    }

    private fun readLoop() {
        try {
            while (open) {
                val f = Wire.read(input) ?: break
                lastHeard.set(System.currentTimeMillis())
                when (f.type) {
                    Type.CMD -> events.onCmd(Cmd.fromJson(JSONObject(f.text)))
                    Type.PING -> queue.offerFirst(Out(Type.PONG, f.payload, false, false, 0))
                    Type.PONG -> if (f.payload.size == 8) {
                        rttMs = (System.nanoTime() / 1000 - ByteBuffer.wrap(f.payload).long) / 1000
                    }
                    Type.BYE -> { shut("OBS closed the connection.", byPeer = true); return }
                    Type.ERROR -> {
                        val o = JSONObject(f.text)
                        shut(o.optString("msg", "The PC refused the connection."), byPeer = true); return
                    }
                }
            }
            shut("The PC closed the connection.", byPeer = true)
        } catch (e: Exception) {
            shut("The connection dropped.", byPeer = false)
        }
    }

    private fun pingLoop() {
        while (open) {
            try { Thread.sleep(PING_MS) } catch (e: InterruptedException) { return }
            if (!open) return
            if (System.currentTimeMillis() - lastHeard.get() > DEAD_MS) {
                shut("The PC stopped answering.", byPeer = false)
                return
            }
            val p = ByteBuffer.allocate(8).putLong(System.nanoTime() / 1000).array()
            queue.offerFirst(Out(Type.PING, p, false, false, 0))
        }
    }

    companion object {
        private const val TAG = "RelyCam"
        const val MAX_QUEUE_US = 250_000L
        const val PING_MS = 2000L
        const val DEAD_MS = 6000L
        private const val CONNECT_MS = 2500

        /**
         * Connects to whichever of the PC's addresses answers first, says HELLO, and waits for
         * WELCOME. Throws [ReachException] naming the most telling failure.
         */
        fun connect(
            pairing: Pairing, hello: Hello, events: LinkEvents, onNeedKeyFrame: () -> Unit,
        ): Link {
            val socket = firstSocket(pairing)
            try {
                socket.tcpNoDelay = true
                socket.sendBufferSize = 512 * 1024
                socket.soTimeout = 5000
                val out = BufferedOutputStream(socket.getOutputStream())
                Wire.write(out, Type.HELLO, hello.toJson().toString().toByteArray(Charsets.UTF_8))
                out.flush()
                val din = DataInputStream(BufferedInputStream(socket.getInputStream()))
                val f = Wire.read(din) ?: throw ReachException(Reach.OTHER, "The PC hung up during pairing.")
                when (f.type) {
                    Type.WELCOME -> Unit
                    Type.ERROR -> {
                        val o = JSONObject(f.text)
                        val r = when (o.optString("code")) {
                            "auth" -> Reach.AUTH
                            "version" -> Reach.VERSION
                            "busy" -> Reach.BUSY
                            else -> Reach.OTHER
                        }
                        throw ReachException(r, o.optString("msg", "The PC refused the connection."))
                    }
                    else -> throw ReachException(Reach.OTHER, "The PC answered with something unexpected.")
                }
                socket.soTimeout = 0
                val link = Link(socket, Welcome.fromJson(JSONObject(f.text)), events, onNeedKeyFrame)
                link.startThreads()
                return link
            } catch (e: ReachException) {
                runCatching { socket.close() }; throw e
            } catch (e: Exception) {
                runCatching { socket.close() }
                throw ReachException(Reach.OTHER, e.message ?: "Pairing failed.")
            }
        }

        private fun firstSocket(p: Pairing): Socket {
            val pool = Executors.newFixedThreadPool(p.hosts.size.coerceAtLeast(1))
            val cs = ExecutorCompletionService<Socket>(pool)
            p.hosts.forEach { h ->
                cs.submit {
                    val s = Socket()
                    try { s.connect(InetSocketAddress(h, p.port), CONNECT_MS); s } catch (e: Exception) { runCatching { s.close() }; throw e }
                }
            }
            val failures = mutableListOf<Throwable>()
            try {
                repeat(p.hosts.size) {
                    val f = cs.poll(CONNECT_MS + 500L, TimeUnit.MILLISECONDS) ?: return@repeat
                    try {
                        val s = f.get()
                        // Close any slower winners that arrive later.
                        pool.submit { repeat(p.hosts.size - 1) { runCatching { cs.poll(CONNECT_MS + 500L, TimeUnit.MILLISECONDS)?.get()?.close() } } }
                        return s
                    } catch (e: Exception) {
                        failures += (e.cause ?: e)
                    }
                }
            } finally {
                pool.shutdown()
            }
            Log.i(TAG, "connect failed: ${failures.map { it.javaClass.simpleName + ":" + it.message }}")
            throw classify(failures)
        }

        private fun classify(fs: List<Throwable>): ReachException = when {
            fs.any { it is ConnectException && it.message.orEmpty().contains("ECONNREFUSED") } ->
                ReachException(Reach.REFUSED, "The PC answered, but nothing is listening. Is OBS open, with Rely-A-Cam installed?")
            fs.any { it is NoRouteToHostException || it.message.orEmpty().contains("ENETUNREACH") || it.message.orEmpty().contains("EHOSTUNREACH") } ->
                ReachException(Reach.NO_ROUTE, "This phone has no route to the PC. Are both on the same Wi-Fi?")
            fs.isEmpty() || fs.all { it is SocketTimeoutException || it.message.orEmpty().contains("timed out") } ->
                ReachException(Reach.TIMEOUT, "The PC did not answer. Windows Firewall is the usual cause.")
            else -> ReachException(Reach.OTHER, fs.first().message ?: "Could not reach the PC.")
        }
    }
}
