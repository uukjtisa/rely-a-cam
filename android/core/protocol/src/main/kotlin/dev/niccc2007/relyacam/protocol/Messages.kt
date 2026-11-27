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
    /** Brightness range and step in EV, `[min, max, step]`; empty when the camera has none. */
    val ev: List<Double> = emptyList(),
    /** White balance choices, a subset of [WhiteBalance.ALL], in that order. */
    val wb: List<String> = emptyList(),
    /** The camera can lock focus and exposure. */
    val aeLock: Boolean = false,
) {
    val canZoom: Boolean get() = max > min + 1e-3

    fun toJson(): JSONObject = JSONObject()
        .put("id", id).put("facing", facing).put("label", label)
        .put("min", round3(min)).put("max", round3(max))
        .put("lenses", JSONArray(lenses.map { round3(it) }))
        .put("flash", flash).put("modes", JSONArray(modes))
        .put("ev", JSONArray(ev.map { round3(it) })).put("wb", JSONArray(wb)).put("aeLock", aeLock)

    companion object {
        fun fromJson(o: JSONObject) = CamCaps(
            o.getString("id"), o.optString("facing"), o.optString("label"),
            o.getDouble("min"), o.getDouble("max"),
            o.optJSONArray("lenses").doubles(), o.optBoolean("flash"),
            o.optJSONArray("modes").strings(),
            o.optJSONArray("ev").doubles(), o.optJSONArray("wb").strings(), o.optBoolean("aeLock"),
        )
    }
}

/** White balance choices (PROTOCOL.md, CAPS `wb`), in the order screens list them. */
object WhiteBalance {
    const val AUTO = "auto"
    const val LOCK = "lock"
    val ALL = listOf(AUTO, LOCK, "daylight", "cloudy", "indoor", "fluorescent")

    fun label(k: String) = when (k) {
        AUTO -> "Auto"; LOCK -> "Lock current"; "daylight" -> "Daylight"; "cloudy" -> "Cloudy"
        "indoor" -> "Indoor"; "fluorescent" -> "Fluorescent"; else -> k
    }
}

/** Where the rule-of-thirds grid is drawn (PROTOCOL.md, STATE `grid`). */
data class Grid(val dock: Boolean = false, val phone: Boolean = false, val out: Boolean = false) {
    fun toJson(): JSONObject = JSONObject().put("dock", dock).put("phone", phone).put("out", out)

    /** This grid with [patch] applied (a CMD carries any subset of the keys). */
    fun with(patch: Map<String, Boolean>) = Grid(
        patch["dock"] ?: dock, patch["phone"] ?: phone, patch["out"] ?: out,
    )

    companion object {
        val PLACES = listOf("dock", "phone", "out")
        fun fromJson(o: JSONObject?) = if (o == null) Grid() else Grid(o.optBoolean("dock"), o.optBoolean("phone"), o.optBoolean("out"))
    }
}

/** One audio input the phone has right now (built-in mics, and wired, USB or Bluetooth when connected). */
data class MicInput(val id: String, val label: String) {
    fun toJson(): JSONObject = JSONObject().put("id", id).put("label", label)

    companion object {
        fun fromJson(o: JSONObject) = MicInput(o.optString("id"), o.optString("label"))
    }
}

/**
 * The phone's capability report. `mics` lists its audio inputs; `clean` lists the voice clean-up
 * effects it has, from "ns" (noise suppression), "aec" (echo cancellation) and "agc" (auto gain).
 */
data class Caps(
    val device: String,
    val cams: List<CamCaps>,
    val codecs: List<String>,
    val mic: Boolean,
    val mics: List<MicInput> = emptyList(),
    val clean: List<String> = emptyList(),
) {
    fun toJson(): JSONObject = JSONObject()
        .put("device", device).put("cams", JSONArray(cams.map { it.toJson() }))
        .put("codecs", JSONArray(codecs)).put("mic", mic)
        .put("mics", JSONArray(mics.map { it.toJson() })).put("clean", JSONArray(clean))

    companion object {
        fun fromJson(o: JSONObject) = Caps(
            o.optString("device"),
            o.optJSONArray("cams")?.let { a -> (0 until a.length()).map { CamCaps.fromJson(a.getJSONObject(it)) } }.orEmpty(),
            o.optJSONArray("codecs").strings(), o.optBoolean("mic"),
            o.optJSONArray("mics")?.let { a -> (0 until a.length()).map { MicInput.fromJson(a.getJSONObject(it)) } }.orEmpty(),
            o.optJSONArray("clean").strings(),
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
    /** The audio input in use (a [MicInput] id), and the clean-up effects that are on. */
    val micIn: String = "",
    val ns: Boolean = false,
    val aec: Boolean = false,
    val agc: Boolean = false,
    /** Camera off: only the mic streams. */
    val micOnly: Boolean = false,
    /** Focus and exposure locked; brightness in EV; white balance choice. */
    val aeLock: Boolean = false,
    val ev: Double = 0.0,
    val wb: String = WhiteBalance.AUTO,
    /** The photo size a snap request would send now ("4096x3072"), or empty when there's none. */
    val snap: String = "",
    val grid: Grid = Grid(),
) {
    fun toJson(): JSONObject = JSONObject()
        .put("cam", cam).put("zoom", round3(zoom)).put("torch", torch).put("mic", mic)
        .put("mode", mode).put("codec", codec).put("rot", rot).put("mirror", mirror)
        .put("flip", flip).put("lock", lock).put("temp", round1(temp)).put("battery", battery)
        .put("live", live).put("micIn", micIn).put("ns", ns).put("aec", aec).put("agc", agc)
        .put("micOnly", micOnly).put("aeLock", aeLock).put("ev", round3(ev)).put("wb", wb)
        .put("snap", snap).put("grid", grid.toJson())

    companion object {
        fun fromJson(o: JSONObject) = CamState(
            o.optString("cam"), o.optDouble("zoom", 1.0), o.optBoolean("torch"), o.optBoolean("mic", true),
            o.optString("mode"), o.optString("codec", "h264"), o.optInt("rot"), o.optBoolean("mirror"),
            o.optBoolean("flip"), o.optBoolean("lock"), o.optDouble("temp", 0.0), o.optInt("battery", -1),
            o.optBoolean("live"), o.optString("micIn"), o.optBoolean("ns"), o.optBoolean("aec"),
            o.optBoolean("agc"), o.optBoolean("micOnly"), o.optBoolean("aeLock"), o.optDouble("ev", 0.0),
            o.optString("wb", WhiteBalance.AUTO), o.optString("snap"), Grid.fromJson(o.optJSONObject("grid")),
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
    val micIn: String? = null,
    val ns: Boolean? = null,
    val aec: Boolean? = null,
    val agc: Boolean? = null,
    val micOnly: Boolean? = null,
    val aeLock: Boolean? = null,
    val ev: Double? = null,
    val wb: String? = null,
    /** Take one photo and send it (SNAP). Never part of `restore`. */
    val snap: Boolean = false,
    /** Any subset of "dock", "phone", "out". */
    val grid: Map<String, Boolean> = emptyMap(),
) {
    fun toJson(): JSONObject = JSONObject().apply {
        cam?.let { put("cam", it) }; zoom?.let { put("zoom", round3(it)) }
        torch?.let { put("torch", it) }; mic?.let { put("mic", it) }
        mode?.let { put("mode", it) }; codec?.let { put("codec", it) }
        mirror?.let { put("mirror", it) }; flip?.let { put("flip", it) }; lock?.let { put("lock", it) }
        if (keyframe) put("keyframe", true)
        if (zooms.isNotEmpty()) put("zooms", JSONObject(zooms.mapValues { round3(it.value) }))
        micIn?.let { put("micIn", it) }; ns?.let { put("ns", it) }; aec?.let { put("aec", it) }
        agc?.let { put("agc", it) }; micOnly?.let { put("micOnly", it) }
        aeLock?.let { put("aeLock", it) }; ev?.let { put("ev", round3(it)) }; wb?.let { put("wb", it) }
        if (snap) put("snap", true)
        if (grid.isNotEmpty()) put("grid", JSONObject(grid))
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
                micIn = o.optString("micIn").takeIf { o.has("micIn") && it.isNotEmpty() },
                ns = b("ns"), aec = b("aec"), agc = b("agc"), micOnly = b("micOnly"),
                aeLock = b("aeLock"), ev = if (o.has("ev")) o.optDouble("ev") else null,
                wb = o.optString("wb").takeIf { o.has("wb") && it.isNotEmpty() },
                snap = o.optBoolean("snap"),
                grid = o.optJSONObject("grid")?.let { g ->
                    Grid.PLACES.filter { g.has(it) }.associateWith { g.optBoolean(it) }
                }.orEmpty(),
            )
        }
    }
}

data class Hello(val secret: String, val device: String, val name: String, val app: String) {
    fun toJson(): JSONObject = JSONObject()
        .put("v", PROTOCOL_VERSION).put("k", secret).put("device", device).put("name", name).put("app", app)
}

data class Welcome(
    val pc: String, val pcId: String, val source: String, val codecs: List<String>, val restore: Cmd,
    /** The PC has Rely-A-Cam-Gestures installed, and whether this phone has gestures on there. */
    val gesturesInstalled: Boolean = false,
    val gesturesOn: Boolean = false,
) {
    companion object {
        fun fromJson(o: JSONObject) = Welcome(
            o.optString("pc"), o.optString("pcId"), o.optString("source"),
            o.optJSONArray("codecs").strings(), Cmd.fromJson(o.optJSONObject("restore")),
            o.optJSONObject("gestures")?.optBoolean("installed") ?: false,
            o.optJSONObject("gestures")?.optBoolean("on") ?: false,
        )
    }
}

/**
 * GDET: what the gestures app sees, to draw over the phone's picture. Points are fractions of the
 * upright picture OBS shows (after mirror and flip), 21 per hand.
 */
data class GestureDet(val hands: List<FloatArray>, val labels: List<Label>, val face: List<FloatArray> = emptyList()) {
    /** [armed] 0..1 is how long it's been held; [confirm] names the confirm sign while [state] is "waiting". */
    data class Label(val name: String, val state: String, val x: Float, val y: Float, val armed: Float = 0f, val confirm: String = "")

    companion object {
        fun fromJson(o: JSONObject): GestureDet {
            val hs = o.optJSONArray("hands")
            val gs = o.optJSONArray("g")
            val fs = o.optJSONArray("face")
            fun lines(a: org.json.JSONArray?) = (0 until (a?.length() ?: 0)).mapNotNull { i -> a!!.optJSONArray(i) }.map { l ->
                FloatArray(l.length()) { k -> l.optDouble(k).toFloat() }
            }
            return GestureDet(
                lines(hs),
                (0 until (gs?.length() ?: 0)).mapNotNull { i -> gs!!.optJSONObject(i) }.map {
                    Label(
                        it.optString("n"), it.optString("s"), it.optDouble("x", 0.5).toFloat(), it.optDouble("y", 0.5).toFloat(),
                        it.optDouble("a", 0.0).toFloat(), it.optString("c"),
                    )
                },
                lines(fs),
            )
        }
    }
}

/** One gesture in the PC's library, as the Library sheet lists it. */
data class GestureItem(
    val id: String, val name: String, val on: Boolean, val mine: Boolean, val action: String,
    /** The control this gesture runs, or empty: then the gesture follows that control's switch. */
    val control: String = "",
)

/** One job on the PC's Controls tab. [move] ones are pinch moves; the others have a [gesture]. */
data class ControlItem(
    val id: String, val label: String, val group: String, val on: Boolean, val move: Boolean, val how: String,
    val gesture: String,
)

/** One meme: where it shows (head, full, spot) and its volume. */
data class MemeItem(val id: String, val name: String, val kind: String, val place: String, val volume: Int, val sound: Boolean)

/** GLIB: the gestures app's library and whether this phone has gestures on. */
data class GestureLib(
    val installed: Boolean = false,
    val on: Boolean = false,
    val linked: Boolean = false,
    val status: String = "",
    val gestures: List<GestureItem> = emptyList(),
    val memes: List<MemeItem> = emptyList(),
    val controls: List<ControlItem> = emptyList(),
) {
    companion object {
        fun fromJson(o: JSONObject): GestureLib {
            val gs = o.optJSONArray("gestures")
            val ms = o.optJSONArray("memes")
            val cs = o.optJSONArray("controls")
            return GestureLib(
                o.optBoolean("installed"), o.optBoolean("on"), o.optBoolean("linked"), o.optString("status"),
                (0 until (gs?.length() ?: 0)).mapNotNull { i -> gs!!.optJSONObject(i) }.map {
                    GestureItem(
                        it.optString("id"), it.optString("name"), it.optBoolean("on"), it.optBoolean("mine"), it.optString("action"),
                        it.optString("control"),
                    )
                },
                (0 until (ms?.length() ?: 0)).mapNotNull { i -> ms!!.optJSONObject(i) }.map {
                    MemeItem(it.optString("id"), it.optString("name"), it.optString("kind"), it.optString("place", "head"),
                        it.optInt("volume", 80), it.optBoolean("sound"))
                },
                (0 until (cs?.length() ?: 0)).mapNotNull { i -> cs!!.optJSONObject(i) }.map {
                    ControlItem(
                        it.optString("id"), it.optString("label"), it.optString("group"), it.optBoolean("on"),
                        it.optBoolean("move"), it.optString("how"), it.optString("gesture"),
                    )
                },
            )
        }
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

/** The heat warning, derived the same way on both sides (PROTOCOL.md, "Heat"). */
object Heat {
    const val HOT_C = 45.0

    fun hot(temp: Double) = temp >= HOT_C

    /**
     * The mode to suggest while hot: the default mode when [current] is above it, else the next
     * smaller one the camera has. Null when nothing smaller exists.
     */
    fun suggest(modes: List<String>, current: String): String? {
        val cur = Mode.parse(current) ?: return null
        val cost = { m: Mode -> m.w.toLong() * m.h * m.fps }
        val parsed = modes.mapNotNull { Mode.parse(it) }
        Mode.default(modes)?.let { Mode.parse(it) }?.takeIf { cost(it) < cost(cur) }?.let { return it.toString() }
        return parsed.filter { cost(it) < cost(cur) }.maxByOrNull { cost(it) }?.toString()
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
