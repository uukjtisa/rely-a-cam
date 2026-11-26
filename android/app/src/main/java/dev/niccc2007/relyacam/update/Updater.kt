package dev.niccc2007.relyacam.update

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.Settings
import androidx.core.content.FileProvider
import dev.niccc2007.relyacam.BuildConfig
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/**
 * The phone app's updater, ported from Filet's (update/Updater.kt there): it reads the project's
 * GitHub releases, and when a newer one carries an APK, downloads and hands it to the installer.
 * No server of our own and no account: it is the same public page a person would open.
 */
data class Release(val tag: String, val name: String, val url: String, val apkUrl: String?, val pluginUrl: String?) {
    val version: Version get() = Version.parse(tag)
}

data class Version(val major: Int = 0, val minor: Int = 0, val patch: Int = 0, val pre: String = "") : Comparable<Version> {
    override fun compareTo(other: Version): Int {
        major.compareTo(other.major).let { if (it != 0) return it }
        minor.compareTo(other.minor).let { if (it != 0) return it }
        patch.compareTo(other.patch).let { if (it != 0) return it }
        return when {
            pre.isEmpty() && other.pre.isEmpty() -> 0
            pre.isEmpty() -> 1
            other.pre.isEmpty() -> -1
            else -> pre.compareTo(other.pre)
        }
    }

    override fun toString() = "$major.$minor.$patch" + if (pre.isEmpty()) "" else "-$pre"

    companion object {
        private val RE = Regex("""v?(\d+)\.(\d+)(?:\.(\d+))?(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?""")
        fun parse(raw: String?): Version {
            val m = RE.find(raw.orEmpty().trim()) ?: return Version()
            return Version(m.groupValues[1].toIntOrNull() ?: 0, m.groupValues[2].toIntOrNull() ?: 0,
                m.groupValues[3].toIntOrNull() ?: 0, m.groupValues[4])
        }
    }
}

object Updater {
    const val OWNER = "uukjtisa"
    const val REPO = "rely-a-cam"
    /** Where a person gets both halves. The PC plugin installer is attached to the same release. */
    const val RELEASES = "https://github.com/$OWNER/$REPO/releases/latest"
    private const val API = "https://api.github.com/repos/$OWNER/$REPO/releases?per_page=10"

    val installed: Version get() = Version.parse(BuildConfig.VERSION_NAME)

    /** The newest non-draft, non-prerelease release, or null. Blocking: call off the main thread. */
    fun latest(): Release? {
        val body = get(API) ?: return null
        val arr = JSONArray(body)
        return (0 until arr.length()).map { arr.getJSONObject(it) }
            .filter { !it.optBoolean("draft") && !it.optBoolean("prerelease") }
            .map { it.toRelease() }
            .maxByOrNull { it.version }
    }

    /** A newer release than this app, or null (also null when offline). */
    fun check(): Release? = runCatching { latest()?.takeIf { it.version > installed && it.apkUrl != null } }.getOrNull()

    /** Downloads the APK, reporting 0..100. Returns the file, or throws with a reason. */
    fun download(context: Context, r: Release, progress: (Int) -> Unit): File {
        val url = r.apkUrl ?: error("That release has no APK attached.")
        val dir = File(context.cacheDir, "updates").apply { mkdirs() }
        dir.listFiles()?.forEach { it.delete() }
        val target = File(dir, "RelyACam-${r.tag}.apk")
        val c = open(url)
        val total = c.contentLengthLong
        c.inputStream.use { input ->
            target.outputStream().use { out ->
                val buf = ByteArray(64 * 1024)
                var done = 0L
                var last = -1
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) break
                    out.write(buf, 0, n)
                    done += n
                    val p = if (total > 0) (done * 100 / total).toInt() else -1
                    if (p != last) { last = p; progress(p) }
                }
                if (total > 0 && done != total) { target.delete(); error("The download was cut off.") }
            }
        }
        return target
    }

    /** Hands the APK to Android's installer, first asking for "install unknown apps" if needed. */
    fun install(context: Context, apk: File): Boolean {
        if (Build.VERSION.SDK_INT >= 26 && !context.packageManager.canRequestPackageInstalls()) {
            context.startActivity(
                Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + context.packageName))
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            )
            return false
        }
        val uri = FileProvider.getUriForFile(context, context.packageName + ".files", apk)
        context.startActivity(
            Intent(Intent.ACTION_VIEW).setDataAndType(uri, "application/vnd.android.package-archive")
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
        )
        return true
    }

    private fun JSONObject.toRelease(): Release {
        val assets = optJSONArray("assets") ?: JSONArray()
        var apk: String? = null
        var plugin: String? = null
        for (i in 0 until assets.length()) {
            val a = assets.getJSONObject(i)
            val name = a.optString("name")
            val dl = a.optString("browser_download_url")
            if (name.endsWith(".apk", true)) apk = dl
            if (name.endsWith(".exe", true)) plugin = dl
        }
        return Release(optString("tag_name"), optString("name"), optString("html_url"), apk, plugin)
    }

    private fun open(url: String): HttpURLConnection {
        var u = URL(url)
        repeat(5) {
            val c = (u.openConnection() as HttpURLConnection).apply {
                connectTimeout = 10_000
                readTimeout = 20_000
                instanceFollowRedirects = false
                setRequestProperty("User-Agent", "Rely-A-Cam/${BuildConfig.VERSION_NAME}")
                setRequestProperty("Accept", "application/vnd.github+json, application/octet-stream")
            }
            if (c.responseCode in 300..399) {
                u = URL(u, c.getHeaderField("Location") ?: error("redirect without a location"))
                c.disconnect()
            } else {
                if (c.responseCode !in 200..299) error("GitHub answered ${c.responseCode}.")
                return c
            }
        }
        error("Too many redirects.")
    }

    private fun get(url: String): String? = runCatching { open(url).inputStream.use { it.readBytes().toString(Charsets.UTF_8) } }.getOrNull()
}
