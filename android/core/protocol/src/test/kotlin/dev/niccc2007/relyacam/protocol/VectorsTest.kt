package dev.niccc2007.relyacam.protocol

import org.json.JSONObject
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.io.File

/** protocol/vectors/vectors.json, the contract shared with the OBS plugin's tests. */
class VectorsTest {

    private val v = JSONObject(File(System.getProperty("vectors"), "vectors.json").readText())

    private fun hex(s: String) = ByteArray(s.length / 2) { s.substring(it * 2, it * 2 + 2).toInt(16).toByte() }

    @Test fun frames() {
        val a = v.getJSONArray("frames")
        for (i in 0 until a.length()) {
            val c = a.getJSONObject(i)
            val payload = when {
                c.has("payload_text") -> c.getString("payload_text").toByteArray()
                c.has("media") -> c.getJSONObject("media").let { m ->
                    Wire.media(m.getLong("pts"), m.getInt("flags"), hex(m.getString("data_hex")))
                }
                else -> hex(c.getString("payload_hex"))
            }
            val bytes = Wire.encode(c.getInt("type"), payload)
            assertArrayEquals(c.getString("name"), hex(c.getString("hex")), bytes)
            val back = Wire.decodeAll(bytes).single()
            assertEquals(c.getInt("type"), back.type)
            assertArrayEquals(payload, back.payload)
        }
    }

    @Test fun pairing() {
        val a = v.getJSONArray("pairing")
        for (i in 0 until a.length()) {
            val c = a.getJSONObject(i)
            val p = Pairing.parse(c.getString("uri"))
            if (c.optBoolean("invalid")) { assertNull(c.getString("uri"), p); continue }
            p!!
            assertEquals((0 until c.getJSONArray("hosts").length()).map { c.getJSONArray("hosts").getString(it) }, p.hosts)
            assertEquals(c.getInt("port"), p.port)
            assertEquals(c.getString("secret"), p.secret)
            assertEquals(c.getString("id"), p.pcId)
            assertEquals(c.getString("name"), p.pcName)
            assertEquals(p, Pairing.parse(p.toUri()))
        }
    }

    @Test fun presets() {
        val a = v.getJSONArray("presets")
        for (i in 0 until a.length()) {
            val c = a.getJSONObject(i)
            val lenses = c.getJSONArray("lenses").let { l -> (0 until l.length()).map { l.getDouble(it) } }
            val cam = CamCaps("0", "back", "Back", c.getDouble("min"), c.getDouble("max"), lenses, false, emptyList())
            val expect = c.getJSONArray("expect").let { l -> (0 until l.length()).map { l.getDouble(it) } }
            assertEquals(c.getString("name"), expect, Presets.of(cam))
        }
    }

    @Test fun defaultMode() {
        val a = v.getJSONArray("defaultMode")
        for (i in 0 until a.length()) {
            val c = a.getJSONObject(i)
            val modes = c.getJSONArray("modes").let { l -> (0 until l.length()).map { l.getString(it) } }
            assertEquals(c.getString("expect"), Mode.default(modes))
        }
    }

    @Test fun codes() {
        val a = v.getJSONArray("codes")
        for (i in 0 until a.length()) {
            val c = a.getJSONObject(i)
            assertEquals(c.getString("raw"), c.getBoolean("ok"), Discovery.isCode(c.getString("raw")))
            if (c.has("normal")) assertEquals(c.getString("normal"), Discovery.normaliseCode(c.getString("raw")))
        }
    }

    @Test fun cmdRoundTrip() {
        val cmd = Cmd(cam = "4", zoom = 2.4, mirror = true, zooms = mapOf("4" to 2.4, "1" to 1.0))
        assertEquals(cmd, Cmd.fromJson(JSONObject(cmd.toJson().toString())))
    }
}
