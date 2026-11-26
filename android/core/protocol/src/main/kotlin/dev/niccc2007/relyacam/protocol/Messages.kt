package dev.niccc2007.relyacam.protocol

import org.json.JSONArray
import org.json.JSONObject
import java.net.URI
import java.net.URLDecoder
import java.net.URLEncoder
import kotlin.math.abs

/** What the QR (or a discovery reply, or USB) carries. */
data class Pairing(
    val hosts: List<String>,
    val port: Int,
    val secret: String,
    val pcId: String,
    val pcName: String,
    val version: Int = PROTOCOL_VERSION,
) {
    fun toUri(): String = buildString {
        append("relyacam://pair?v=").append(version)
        append("&h=").append(hosts.joinToString(","))
        append("&p=").append(port)
        append("&k=").append(secret)
        append("&id=").append(pcId)
        append("&n=").append(URLEncoder.encode(pcName, "UTF-8").replace("+", "%20"))
    }

    fun toJson(): JSONObject = JSONObject()
        .put("v", version).put("h", JSONArray(hosts)).put("p", port)
        .put("k", secret).put("id", pcId).put("n", pcName)

    companion object {
        /** Null when [text] is not a Rely-A-Cam pairing link (any other QR is ignored, not an error). */
        fun parse(text: String): Pairing? {
            val t = text.trim()
            if (!t.startsWith("relyacam://pair", ignoreCase = true)) return null
            val query = runCatching { URI(t).rawQuery }.getOrNull() ?: t.substringAfter('?', "")
            val q = query.split('&').mapNotNull {
                val i = it.indexOf('=')
                if (i <= 0) null else it.substring(0, i) to URLDecoder.decode(it.substring(i + 1), "UTF-8")
            }.toMap()
            val hosts = q["h"]?.split(',')?.map { it.trim() }?.filter { it.isNotEmpty() }.orEmpty()
            val port = q["p"]?.toIntOrNull() ?: return null
            val secret = q["k"]?.takeIf { it.length >= 16 } ?: return null
            if (hosts.isEmpty() || port !in 1..65535) return null
            return Pairing(hosts, port, secret, q["id"].orEmpty(), q["n"].orEmpty(), q["v"]?.toIntOrNull() ?: 1)
        }

        fun fromJson(o: JSONObject): Pairing? {
            val h = o.optJSONArray("h") ?: return null
            val hosts = (0 until h.length()).map { h.getString(it) }
            val k = o.optString("k").takeIf { it.isNotEmpty() } ?: return null
            return Pairing(hosts, o.optInt("p", DEFAULT_PORT), k, o.optString("id"), o.optString("n"), o.optInt("v", 1))
        }
    }
}

/** One camera in the capability report. Every number comes from the phone's Camera2 answers. */
data class CamCaps(
    val id: String,
    val facing: String,
    val label: String,
    val min: Double,
    val max: Double,
    val lenses: List<Double>,
    val flash: Boolean,
    val modes: List<String>,
) {
    val canZoom: Boolean get() = max > min + 1e-3

    fun toJson(): JSONObject = JSONObject()
        .put("id", id).put("facing", facing).put("label", label)
        .put("min", round3(min)).put("max", round3(max))
        .put("lenses", JSONArray(lenses.map { round3(it) }))
        .put("flash", flash).put("modes", JSONArray(modes))

    companion object {
        fun fromJson(o: JSONObject) = CamCaps(
            o.getString("id"), o.optString("facing"), o.optString("label"),
            o.getDouble("min"), o.getDouble("max"),
            o.optJSONArray("lenses").doubles(), o.optBoolean("flash"),
            o.optJSONArray("modes").strings(),
        )
    }
}

data class Caps(val device: String, val cams: List<CamCaps>, val codecs: List<String>, val mic: Boolean) {
    fun toJson(): JSONObject = JSONObject()
        .put("device", device).put("cams", JSONArray(cams.map { it.toJson() }))
        .put("codecs", JSONArray(codecs)).put("mic", mic)

    companion object {
        fun fromJson(o: JSONObject) = Caps(
            o.optString("device"),
            o.optJSONArray("cams")?.let { a -> (0 until a.length()).map { CamCaps.fromJson(a.getJSONObject(it)) } }.orEmpty(),
            o.optJSONArray("codecs").strings(), o.optBoolean("mic"),
        )
    }
}

/** The phone's full state (STATE). The phone owns every value here. */
data class CamState(
    val cam: String = "",
    val zoom: Double = 1.0,
    val torch: Boolean = false,
    val mic: Boolean = true,
    val mode: String = "",
    val codec: String = "h264",
    val rot: Int = 0,
    val mirror: Boolean = false,
    val flip: Boolean = false,
    val lock: Boolean = false,
    val temp: Double = 0.0,
    val battery: Int = -1,
    val live: Boolean = false,
) {
    fun toJson(): JSONObject = JSONObject()
        .put("cam", cam).put("zoom", round3(zoom)).put("torch", torch).put("mic", mic)
        .put("mode", mode).put("codec", codec).put("rot", rot).put("mirror", mirror)
        .put("flip", flip).put("lock", lock).put("temp", round1(temp)).put("battery", battery)
        .put("live", live)

    companion object {
        fun fromJson(o: JSONObject) = CamState(
            o.optString("cam"), o.optDouble("zoom", 1.0), o.optBoolean("torch"), o.optBoolean("mic", true),
            o.optString("mode"), o.optString("codec", "h264"), o.optInt("rot"), o.optBoolean("mirror"),
            o.optBoolean("flip"), o.optBoolean("lock"), o.optDouble("temp", 0.0), o.optInt("battery", -1),
            o.optBoolean("live"),
        )
    }
}

/**
 * A request from the PC (CMD), or OBS's remembered settings (WELCOME `restore`). Every field is
 * optional: only what is present changes. `zooms` is the remembered zoom per camera id.
 */
data class Cmd(
    val cam: String? = null,
    val zoom: Double? = null,
    val torch: Boolean? = null,
    val mic: Boolean? = null,
    val mode: String? = null,
    val codec: String? = null,
    val mirror: Boolean? = null,
    val flip: Boolean? = null,
    val lock: Boolean? = null,
    val keyframe: Boolean = false,
    val zooms: Map<String, Double> = emptyMap(),
) {
    fun toJson(): JSONObject = JSONObject().apply {
        cam?.let { put("cam", it) }; zoom?.let { put("zoom", round3(it)) }
        torch?.let { put("torch", it) }; mic?.let { put("mic", it) }
        mode?.let { put("mode", it) }; codec?.let { put("codec", it) }
        mirror?.let { put("mirror", it) }; flip?.let { put("flip", it) }; lock?.let { put("lock", it) }
        if (keyframe) put("keyframe", true)
        if (zooms.isNotEmpty()) put("zooms", JSONObject(zooms.mapValues { round3(it.value) }))
    }

    companion object {
        fun fromJson(o: JSONObject?): Cmd {
            if (o == null) return Cmd()
            fun b(k: String) = if (o.has(k)) o.optBoolean(k) else null
            val zs = o.optJSONObject("zooms")
            return Cmd(
                cam = o.optString("cam").takeIf { o.has("cam") && it.isNotEmpty() },
                zoom = if (o.has("zoom")) o.optDouble("zoom") else null,
                torch = b("torch"), mic = b("mic"),
                mode = o.optString("mode").takeIf { o.has("mode") && it.isNotEmpty() },
                codec = o.optString("codec").takeIf { o.has("codec") && it.isNotEmpty() },
                mirror = b("mirror"), flip = b("flip"), lock = b("lock"),
                keyframe = o.optBoolean("keyframe"),
                zooms = zs?.keys()?.asSequence()?.associateWith { zs.optDouble(it) }.orEmpty(),
            )
        }
    }
}

data class Hello(val secret: String, val device: String, val name: String, val app: String) {
    fun toJson(): JSONObject = JSONObject()
        .put("v", PROTOCOL_VERSION).put("k", secret).put("device", device).put("name", name).put("app", app)
}

data class Welcome(val pc: String, val pcId: String, val source: String, val codecs: List<String>, val restore: Cmd) {
    companion object {
        fun fromJson(o: JSONObject) = Welcome(
            o.optString("pc"), o.optString("pcId"), o.optString("source"),
            o.optJSONArray("codecs").strings(), Cmd.fromJson(o.optJSONObject("restore")),
        )
    }
}

/** A video or audio mode string, "1920x1080@30". */
data class Mode(val w: Int, val h: Int, val fps: Int) {
    override fun toString() = "${w}x$h@$fps"
    val label: String get() = (if (w >= 3840) "4K" else "${minOf(w, h)}p") + " $fps"

    companion object {
        private val RE = Regex("""(\d+)x(\d+)@(\d+)""")
        fun parse(s: String): Mode? = RE.matchEntire(s.trim())?.let {
            Mode(it.groupValues[1].toInt(), it.groupValues[2].toInt(), it.groupValues[3].toInt())
        }

        /**
         * The mode a new stream starts in: 1080p 30 when the phone has it, else the best below it,
         * else the smallest it has. Never a hardcoded size the phone did not report.
         */
        fun default(modes: List<String>): String? {
            val parsed = modes.mapNotNull { parse(it) }
            if (parsed.isEmpty()) return null
            val pixels = 1920 * 1080
            val atOrBelow = parsed.filter { it.w * it.h <= pixels && it.fps <= 30 }
            val pick = atOrBelow.maxWithOrNull(compareBy({ it.w * it.h }, { it.fps }))
                ?: parsed.minWithOrNull(compareBy({ it.w * it.h }, { it.fps }))
            return pick.toString()
        }
    }
}

/** Preset zoom stops, derived from the report the same way on both sides (PROTOCOL.md). */
object Presets {
    fun of(cam: CamCaps): List<Double> {
        if (!cam.canZoom) return emptyList()
        val stops = cam.lenses.filter { it >= cam.min - 1e-3 && it <= cam.max + 1e-3 }.toMutableList()
        if (2.0 >= cam.min && 2.0 <= cam.max && stops.none { abs(it - 2.0) / 2.0 < 0.2 }) stops += 2.0
        return stops.distinctBy { round3(it) }.sorted()
    }

    /** "0.65x", "1x", "2.5x". */
    fun label(z: Double): String {
        val r = Math.round(z * 100) / 100.0
        val s = if (r == Math.floor(r)) r.toLong().toString() else r.toString().trimEnd('0').trimEnd('.')
        return "${s}x"
    }
}

/** UDP discovery datagrams (PROTOCOL.md "Discovery"). */
object Discovery {
    const val ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"

    fun codeRequest(code: String): ByteArray = "RAC1?C${normaliseCode(code)}".toByteArray(Charsets.US_ASCII)
    fun idRequest(pcId: String): ByteArray = "RAC1?I$pcId".toByteArray(Charsets.US_ASCII)

    /** Upper-cases, drops the dash and spaces; null when it can't be a code. */
    fun normaliseCode(raw: String): String {
        val c = raw.uppercase().filter { it != '-' && !it.isWhitespace() }
        return c
    }

    fun isCode(raw: String): Boolean {
        val c = normaliseCode(raw)
        return c.length == 6 && c.all { it in ALPHABET }
    }
}

private fun round3(v: Double) = Math.round(v * 1000) / 1000.0
private fun round1(v: Double) = Math.round(v * 10) / 10.0
private fun JSONArray?.doubles(): List<Double> = if (this == null) emptyList() else (0 until length()).map { getDouble(it) }
private fun JSONArray?.strings(): List<String> = if (this == null) emptyList() else (0 until length()).map { getString(it) }
