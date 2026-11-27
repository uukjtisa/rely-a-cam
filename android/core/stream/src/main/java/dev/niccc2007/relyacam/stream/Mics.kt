package dev.niccc2007.relyacam.stream

import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.media.audiofx.AcousticEchoCanceler
import android.media.audiofx.AutomaticGainControl
import android.media.audiofx.NoiseSuppressor
import android.os.Build

/** An input the phone can record from right now, with an id that stays the same across plug-ins. */
data class MicSource(val id: String, val label: String, val device: AudioDeviceInfo) {
    val bluetooth: Boolean get() = device.type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO || device.type == TYPE_BLE_HEADSET
}

/** Voice clean-up switches, each applied only when the phone has that effect. */
data class Fx(val ns: Boolean = false, val aec: Boolean = false, val agc: Boolean = false)

/** AudioDeviceInfo.TYPE_BLE_HEADSET (API 31), spelled out so older SDK levels compile. */
internal const val TYPE_BLE_HEADSET = 26

/**
 * What the phone reports about its microphones. Nothing here is hardcoded per phone: the list is
 * whatever Android says is connected, and the effects are whatever it says it has.
 */
object Mics {
    private val KEEP = setOf(
        AudioDeviceInfo.TYPE_BUILTIN_MIC, AudioDeviceInfo.TYPE_WIRED_HEADSET, AudioDeviceInfo.TYPE_USB_DEVICE,
        AudioDeviceInfo.TYPE_USB_HEADSET, AudioDeviceInfo.TYPE_USB_ACCESSORY, AudioDeviceInfo.TYPE_BLUETOOTH_SCO,
        TYPE_BLE_HEADSET,
    )

    /**
     * Built-in mics first, in the order the phone lists them, then anything plugged in. The id is
     * "type:address" (the address names the mic, like "bottom" or "back"), so a choice survives the
     * device ids Android hands out again on every plug-in.
     */
    fun list(am: AudioManager): List<MicSource> {
        val out = mutableListOf<MicSource>()
        val count = mutableMapOf<Int, Int>()
        for (d in am.getDevices(AudioManager.GET_DEVICES_INPUTS)) {
            if (d.type !in KEEP) continue
            val addr = if (Build.VERSION.SDK_INT >= 28) d.address.orEmpty() else ""
            val n = (count[d.type] ?: 0) + 1
            count[d.type] = n
            val id = "${d.type}:" + addr.ifEmpty { "$n" }
            if (out.any { it.id == id }) continue
            out += MicSource(id, label(d, addr, n), d)
        }
        return out.sortedBy { if (it.device.type == AudioDeviceInfo.TYPE_BUILTIN_MIC) 0 else 1 }
    }

    private fun label(d: AudioDeviceInfo, addr: String, n: Int): String {
        val name = d.productName?.toString()?.trim().orEmpty()
        return when (d.type) {
            AudioDeviceInfo.TYPE_BUILTIN_MIC -> {
                val a = addr.lowercase()
                when {
                    "bottom" in a -> "Bottom mic"
                    "back" in a -> "Back mic"
                    "top" in a -> "Top mic"
                    "front" in a -> "Front mic"
                    n > 1 -> "Phone mic $n"
                    else -> "Phone mic"
                }
            }
            AudioDeviceInfo.TYPE_WIRED_HEADSET -> "Wired mic"
            AudioDeviceInfo.TYPE_BLUETOOTH_SCO, TYPE_BLE_HEADSET -> if (name.isNotEmpty()) "Bluetooth: $name" else "Bluetooth mic"
            else -> if (name.isNotEmpty()) "USB: $name" else "USB mic"
        }
    }

    /** The clean-up effects this phone has: "ns", "aec", "agc" (PROTOCOL.md, CAPS). */
    fun effects(): List<String> = buildList {
        if (NoiseSuppressor.isAvailable()) add("ns")
        if (AcousticEchoCanceler.isAvailable()) add("aec")
        if (AutomaticGainControl.isAvailable()) add("agc")
    }
}
