package dev.niccc2007.relyacam.stream

import android.content.Context
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import dev.niccc2007.relyacam.protocol.DISCOVERY_PORT
import dev.niccc2007.relyacam.protocol.Discovery
import dev.niccc2007.relyacam.protocol.Pairing
import org.json.JSONObject
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.NetworkInterface
import java.net.SocketTimeoutException

/**
 * UDP discovery (PROTOCOL.md "Discovery"): finds a PC from a typed code, or finds a paired PC
 * again after its address changed. Broadcast on every network this phone is on, including its
 * own hotspot.
 */
object Finder {

    /** The pairing a 6-character code stands for, or null when no PC answered in time. */
    fun byCode(code: String, timeoutMs: Int = 2500): Pairing? =
        ask(Discovery.codeRequest(code), timeoutMs)?.let { Pairing.fromJson(it) }

    /** Fresh addresses for a PC already paired: the reply has no secret, so the old one is kept. */
    fun byId(old: Pairing, timeoutMs: Int = 1500): Pairing? {
        val o = ask(Discovery.idRequest(old.pcId), timeoutMs) ?: return null
        val h = o.optJSONArray("h") ?: return null
        val hosts = (0 until h.length()).map { h.getString(it) }
        return old.copy(hosts = hosts, port = o.optInt("p", old.port))
    }

    private fun ask(payload: ByteArray, timeoutMs: Int): JSONObject? {
        DatagramSocket().use { s ->
            s.broadcast = true
            s.soTimeout = 400
            val targets = broadcastAddresses() + InetAddress.getByName("255.255.255.255")
            val deadline = System.currentTimeMillis() + timeoutMs
            var nextSend = 0L
            val buf = ByteArray(4096)
            while (System.currentTimeMillis() < deadline) {
                if (System.currentTimeMillis() >= nextSend) {
                    targets.forEach { runCatching { s.send(DatagramPacket(payload, payload.size, it, DISCOVERY_PORT)) } }
                    nextSend = System.currentTimeMillis() + 600
                }
                try {
                    val p = DatagramPacket(buf, buf.size)
                    s.receive(p)
                    val text = String(p.data, 0, p.length, Charsets.UTF_8)
                    return runCatching { JSONObject(text) }.getOrNull() ?: continue
                } catch (e: SocketTimeoutException) {
                    continue
                }
            }
        }
        return null
    }

    private fun broadcastAddresses(): List<InetAddress> = runCatching {
        NetworkInterface.getNetworkInterfaces().toList()
            .filter { it.isUp && !it.isLoopback }
            .flatMap { it.interfaceAddresses }
            .filter { it.address is Inet4Address }
            .mapNotNull { it.broadcast }
            .distinct()
    }.getOrDefault(emptyList())

    /** This phone's own IPv4 addresses, for "are we on the same network?" checks. */
    fun localAddresses(): List<Pair<Inet4Address, Int>> = runCatching {
        NetworkInterface.getNetworkInterfaces().toList()
            .filter { it.isUp && !it.isLoopback }
            .flatMap { it.interfaceAddresses }
            .mapNotNull { a -> (a.address as? Inet4Address)?.let { it to a.networkPrefixLength.toInt() } }
    }.getOrDefault(emptyList())

    /** True when one of [hosts] is inside one of this phone's own subnets. */
    fun sharesSubnet(hosts: List<String>): Boolean {
        val mine = localAddresses()
        return hosts.any { h ->
            val a = runCatching { InetAddress.getByName(h) as? Inet4Address }.getOrNull() ?: return@any false
            if (a.isLoopbackAddress) return@any true
            mine.any { (m, prefix) -> sameNet(a, m, prefix) }
        }
    }

    private fun sameNet(a: Inet4Address, b: Inet4Address, prefix: Int): Boolean {
        val x = a.address; val y = b.address
        var bits = prefix
        for (i in 0 until 4) {
            val mask = if (bits >= 8) 0xFF else if (bits <= 0) 0 else (0xFF shl (8 - bits)) and 0xFF
            if ((x[i].toInt() and mask) != (y[i].toInt() and mask)) return false
            bits -= 8
        }
        return true
    }

    /** "Wi-Fi", "hotspot", "USB" or "mobile data": how the link is carried, for the status pill. */
    fun transportName(context: Context, host: String?): String {
        if (host == "127.0.0.1") return "USB"
        val cm = context.getSystemService(ConnectivityManager::class.java) ?: return "Wi-Fi"
        val caps = cm.getNetworkCapabilities(cm.activeNetwork)
        return when {
            caps == null -> "hotspot"
            caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) -> "Wi-Fi"
            caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) -> "Ethernet"
            caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR) -> "hotspot"
            else -> "Wi-Fi"
        }
    }
}
