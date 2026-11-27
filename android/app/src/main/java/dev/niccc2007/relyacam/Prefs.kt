package dev.niccc2007.relyacam

import android.content.Context
import android.provider.Settings
import dev.niccc2007.relyacam.protocol.Pairing
import org.json.JSONArray
import org.json.JSONObject
import java.util.UUID

/** A PC this phone has paired with, newest first. Kept so it can reconnect without a scan. */
data class SavedPc(val pairing: Pairing, val lastUsed: Long)

/**
 * What the phone remembers between runs: its own id, the PCs it paired with, and the last one it
 * streamed to. Camera settings are not kept here: OBS remembers them per source and sends them
 * back on connect, because OBS wins on reconnect.
 */
class Prefs(context: Context) {
    private val sp = context.getSharedPreferences("relyacam", Context.MODE_PRIVATE)

    /** A random id for this phone, stable across runs, so OBS gives it back the same source. */
    val deviceId: String = sp.getString("deviceId", null) ?: UUID.randomUUID().toString().also {
        sp.edit().putString("deviceId", it).apply()
    }

    val deviceName: String = runCatching {
        Settings.Global.getString(context.contentResolver, Settings.Global.DEVICE_NAME)
    }.getOrNull()?.takeIf { it.isNotBlank() } ?: android.os.Build.MODEL

    fun pcs(): List<SavedPc> = runCatching {
        val a = JSONArray(sp.getString("pcs", "[]"))
        (0 until a.length()).mapNotNull { i ->
            val o = a.getJSONObject(i)
            Pairing.fromJson(o.getJSONObject("p"))?.let { SavedPc(it, o.optLong("t")) }
        }.sortedByDescending { it.lastUsed }
    }.getOrDefault(emptyList())

    fun remember(p: Pairing) {
        val rest = pcs().filter { it.pairing.pcId != p.pcId || p.pcId.isEmpty() && it.pairing.hosts != p.hosts }
        val all = listOf(SavedPc(p, System.currentTimeMillis())) + rest
        val a = JSONArray(all.take(8).map { JSONObject().put("p", it.pairing.toJson()).put("t", it.lastUsed) })
        sp.edit().putString("pcs", a.toString()).apply()
    }

    fun forget(pcId: String) {
        val a = JSONArray(pcs().filter { it.pairing.pcId != pcId }.map { JSONObject().put("p", it.pairing.toJson()).put("t", it.lastUsed) })
        sp.edit().putString("pcs", a.toString()).apply()
    }

    /** True while a stream is running; read on the next launch to decide whether to reconnect. */
    var wasLive: Boolean
        get() = sp.getBoolean("wasLive", false)
        set(v) { sp.edit().putBoolean("wasLive", v).apply() }

    /** The level line on the live screen: a phone-only aid, so the phone keeps it, not OBS. */
    var levelLine: Boolean
        get() = sp.getBoolean("levelLine", false)
        set(v) { sp.edit().putBoolean("levelLine", v).apply() }

    var lastUpdateCheck: Long
        get() = sp.getLong("lastUpdateCheck", 0)
        set(v) { sp.edit().putLong("lastUpdateCheck", v).apply() }
}
