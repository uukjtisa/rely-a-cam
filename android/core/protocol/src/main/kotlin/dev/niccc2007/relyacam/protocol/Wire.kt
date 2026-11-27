package dev.niccc2007.relyacam.protocol

import java.io.DataInputStream
import java.io.EOFException
import java.io.InputStream
import java.io.OutputStream
import java.nio.ByteBuffer

/** Frame types, as listed in protocol/PROTOCOL.md. */
object Type {
    const val HELLO = 0x01
    const val WELCOME = 0x02
    const val CAPS = 0x03
    const val STATE = 0x04
    const val CMD = 0x05
    const val PING = 0x06
    const val PONG = 0x07
    const val BYE = 0x08
    const val ERROR = 0x0F
    const val VCONF = 0x10
    const val VIDEO = 0x11
    const val ACONF = 0x12
    const val AUDIO = 0x13
    const val SNAP = 0x14
}

const val PROTOCOL_VERSION = 1
const val DEFAULT_PORT = 41127
const val DISCOVERY_PORT = 41126
const val MAX_FRAME = 16 * 1024 * 1024

const val FLAG_KEY = 1
const val FLAG_CONFIG = 2

class Frame(val type: Int, val payload: ByteArray) {
    val text: String get() = String(payload, Charsets.UTF_8)
}

/** Big-endian `type:u8 length:u32 payload`. One owner for both directions of the byte layout. */
object Wire {

    fun header(type: Int, length: Int): ByteArray =
        ByteBuffer.allocate(5).put(type.toByte()).putInt(length).array()

    fun encode(type: Int, payload: ByteArray): ByteArray =
        header(type, payload.size) + payload

    fun encodeText(type: Int, text: String): ByteArray = encode(type, text.toByteArray(Charsets.UTF_8))

    /** A VIDEO or AUDIO payload: `pts:u64 flags:u8 data`. */
    fun media(ptsUs: Long, flags: Int, data: ByteArray, offset: Int = 0, length: Int = data.size): ByteArray {
        val out = ByteArray(9 + length)
        ByteBuffer.wrap(out).putLong(ptsUs).put(flags.toByte())
        System.arraycopy(data, offset, out, 9, length)
        return out
    }

    fun write(out: OutputStream, type: Int, payload: ByteArray) {
        out.write(header(type, payload.size))
        out.write(payload)
    }

    /** The next frame, or null at a clean end of stream. Throws on a malformed one. */
    fun read(input: InputStream): Frame? {
        val din = if (input is DataInputStream) input else DataInputStream(input)
        val type = din.read()
        if (type < 0) return null
        val len = try { din.readInt() } catch (e: EOFException) { throw ProtocolException("cut off inside a frame header") }
        if (len < 0 || len > MAX_FRAME) throw ProtocolException("frame of $len bytes is over the limit")
        val payload = ByteArray(len)
        din.readFully(payload)
        return Frame(type, payload)
    }

    /** Splits a byte array holding whole frames (used by the vectors). */
    fun decodeAll(bytes: ByteArray): List<Frame> {
        val input = DataInputStream(bytes.inputStream())
        return buildList { while (true) add(read(input) ?: break) }
    }
}

class ProtocolException(message: String) : Exception(message)
