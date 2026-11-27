package dev.niccc2007.relyacam.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import dev.niccc2007.relyacam.Engine
import dev.niccc2007.relyacam.Screen
import dev.niccc2007.relyacam.UiState
import dev.niccc2007.relyacam.camera.CameraInfo
import dev.niccc2007.relyacam.protocol.CamCaps
import dev.niccc2007.relyacam.protocol.Cmd
import dev.niccc2007.relyacam.protocol.Mode
import dev.niccc2007.relyacam.protocol.Presets
import dev.niccc2007.relyacam.stream.Reach
import dev.niccc2007.relyacam.update.Updater
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.math.ln
import kotlin.math.exp

@Composable
fun App(s: UiState, requestPermissions: () -> Unit) {
    RelyTheme {
        Box(Modifier.fillMaxSize().background(Palette.bg)) {
            if (!s.cameraAllowed) { PermissionScreen(requestPermissions); return@Box }
            when (s.screen) {
                Screen.SCAN -> ScanScreen(s)
                Screen.CONNECTING -> CenterMessage("Connecting to ${s.pcName}…", spinner = true) { Secondary("Cancel") { Engine.cancel() } }
                Screen.LIVE -> LiveScreen(s)
                Screen.UNREACHABLE -> UnreachableScreen(s)
                Screen.WAITING -> CenterMessage(
                    s.problem?.message ?: "Waiting for the PC…",
                    sub = "Rely-A-Cam reconnects on its own when ${s.pcName.ifEmpty { "the PC" }} is back. The camera is off until then.",
                    spinner = true,
                ) { Secondary("Stop waiting") { Engine.cancel() } }
                Screen.GET_PLUGIN -> GetPluginScreen()
            }
            s.notice?.let { Notice(it) }
        }
    }
}

// ---- scan ------------------------------------------------------------------------------------

@Composable
private fun ScanScreen(s: UiState) {
    var codeOpen by remember { mutableStateOf(false) }
    Box(Modifier.fillMaxSize()) {
        CameraView(s, applyMirror = false)
        ScanFrame()
        Column(Modifier.fillMaxSize().safeDrawingPadding().padding(16.dp)) {
            UpdateBanner(s)
            Text("Scan the QR in OBS", color = Color.White, fontSize = 20.sp, fontWeight = FontWeight.SemiBold)
            Text("OBS → Phone Camera dock → Add phone", color = Palette.muted, fontSize = 13.sp)
            Spacer(Modifier.weight(1f))
            s.cam?.let { cam -> ZoomRow(toCaps(cam), s.zoom) { Engine.apply(Cmd(zoom = it)) } }
            Spacer(Modifier.height(10.dp))
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                if (s.cam?.flash == true) RailButton(if (s.torch) "Torch\non" else "Torch", on = s.torch) { Engine.apply(Cmd(torch = !s.torch)) }
                Secondary("Enter code") { codeOpen = true }
                Secondary("Get PC plugin") { Engine.showGetPlugin(true) }
            }
            if (s.recent.isNotEmpty()) {
                Spacer(Modifier.height(10.dp))
                Text("Recent PCs", color = Palette.muted, fontSize = 12.sp)
                s.recent.take(3).forEach { pc ->
                    Row(
                        Modifier.fillMaxWidth().padding(top = 6.dp).clip(RoundedCornerShape(6.dp)).background(Palette.scrim)
                            .clickable { Engine.connectSaved(pc.pairing) }.padding(horizontal = 12.dp, vertical = 10.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(pc.pairing.pcName.ifEmpty { pc.pairing.hosts.first() }, color = Color.White, modifier = Modifier.weight(1f))
                        Text("Reconnect", color = Palette.blueLight, fontSize = 13.sp)
                        Gap(12)
                        Text("Forget", color = Palette.muted, fontSize = 13.sp, modifier = Modifier.clickable { Engine.forget(pc.pairing.pcId) })
                    }
                }
            }
        }
    }
    if (codeOpen) CodeDialog { code -> codeOpen = false; if (code != null) Engine.connectByCode(code) }
}

@Composable
private fun ScanFrame() {
    val t = rememberInfiniteTransition(label = "scan")
    val y by t.animateFloat(0f, 1f, infiniteRepeatable(tween(1800, easing = LinearEasing), RepeatMode.Reverse), label = "line")
    Canvas(Modifier.fillMaxSize()) {
        val side = size.minDimension * 0.62f
        val l = (size.width - side) / 2
        val top = (size.height - side) / 2
        val arm = side * 0.14f
        val w = 4.dp.toPx()
        val c = Color.White
        fun corner(x: Float, yy: Float, dx: Float, dy: Float) {
            drawLine(c, Offset(x, yy), Offset(x + dx * arm, yy), w, StrokeCap.Round)
            drawLine(c, Offset(x, yy), Offset(x, yy + dy * arm), w, StrokeCap.Round)
        }
        corner(l, top, 1f, 1f); corner(l + side, top, -1f, 1f)
        corner(l, top + side, 1f, -1f); corner(l + side, top + side, -1f, -1f)
        val ly = top + side * y
        drawLine(Palette.blueLight.copy(alpha = 0.8f), Offset(l + 8, ly), Offset(l + side - 8, ly), 2.dp.toPx())
    }
}

@Composable
private fun CodeDialog(done: (String?) -> Unit) {
    var code by remember { mutableStateOf("") }
    AlertDialog(
        onDismissRequest = { done(null) },
        title = { Text("Enter the code") },
        text = {
            Column {
                Text("It's under the QR in OBS's Add phone panel.", color = Palette.muted, fontSize = 13.sp)
                Spacer(Modifier.height(8.dp))
                OutlinedTextField(code, { code = it.uppercase().take(7) }, singleLine = true, placeholder = { Text("K7Q-2MX") })
            }
        },
        confirmButton = { TextButton(onClick = { done(code) }) { Text("Connect") } },
        dismissButton = { TextButton(onClick = { done(null) }) { Text("Cancel") } },
    )
}

// ---- live ------------------------------------------------------------------------------------

@Composable
private fun LiveScreen(s: UiState) {
    var setup by remember { mutableStateOf(false) }
    val cam = s.cam ?: return
    Box(Modifier.fillMaxSize()) {
        CameraView(s, applyMirror = true)
        if (s.micOnly) MicOnlyScreen(s)
        Box(Modifier.fillMaxSize().safeDrawingPadding().padding(12.dp)) {
            Column(Modifier.align(Alignment.TopStart), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                if (s.reconnecting) Pill("Reconnecting to ${s.pcName}…", dot = Palette.yellow)
                else Pill("Live to OBS · ${s.transport} · ${s.pcName}", dot = Palette.red)
                Pill(statusLine(s) + " · ⚙ Setup", Modifier.clickable { setup = true })
            }
            Column(
                Modifier.align(Alignment.CenterEnd).verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                // With the camera off, only what still works stays on the rail (DESIGN.md rule 7).
                if (!s.micOnly && cam.flash) RailButton("Torch", on = s.torch) { Engine.apply(Cmd(torch = !s.torch)) }
                if (!s.micOnly && s.cams.size > 1) RailButton("Camera\n" + nextCam(s).label.take(7)) { Engine.apply(Cmd(cam = nextCam(s).id)) }
                RailButton(if (s.mic) "Mic" else "Mic\noff", on = s.mic, enabled = s.micAllowed) { Engine.apply(Cmd(mic = !s.mic)) }
                if (!s.micOnly) {
                    RailButton("Mirror", on = s.mirror) { Engine.apply(Cmd(mirror = !s.mirror)) }
                    RailButton("Flip", on = s.flip) { Engine.apply(Cmd(flip = !s.flip)) }
                    RailButton(if (s.lock) "Locked" else "Lock\nturn", on = s.lock) { Engine.apply(Cmd(lock = !s.lock)) }
                }
                RailButton("Setup") { setup = true }
                RailButton("Dark") { Engine.setBlack(true) }
            }
            if (!s.micOnly) Column(
                Modifier.align(Alignment.BottomCenter).fillMaxWidth(0.7f),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Pill(Presets.label(s.zoom))
                Spacer(Modifier.height(8.dp))
                ZoomRow(toCaps(cam), s.zoom) { Engine.apply(Cmd(zoom = it)) }
                if (cam.zoomMax > cam.zoomMin + 1e-3) ZoomSlider(cam, s.zoom)
            }
            Box(Modifier.align(Alignment.BottomStart)) {
                Primary("Stop") { Engine.stop() }
            }
        }
        if (setup) SetupSheet(s) { setup = false }
        if (s.black) BlackScreen(s)
    }
}

/** Mic only: the camera is off, so the screen shows the mic instead of a frozen picture. */
@Composable
private fun MicOnlyScreen(s: UiState) {
    val lvl by Engine.level.collectAsState()
    val input = s.mics.firstOrNull { it.id == s.micIn }?.label ?: "Phone mic"
    Column(
        Modifier.fillMaxSize().background(Palette.bg).padding(horizontal = 80.dp),
        verticalArrangement = Arrangement.Center, horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text("Mic only", color = Color.White, fontSize = 18.sp, fontWeight = FontWeight.SemiBold)
        Text(if (s.mic) "Camera off · $input" else "Camera off · mic muted", color = Palette.muted, fontSize = 13.sp)
        Spacer(Modifier.height(14.dp))
        Meter(lvl.peak, lvl.clip, Modifier.fillMaxWidth(0.6f), height = 10.dp)
        Spacer(Modifier.height(18.dp))
        Primary("Turn the camera back on") { Engine.apply(Cmd(micOnly = false)) }
    }
}

private val CLEAN_LABEL = mapOf("ns" to "Noise", "aec" to "Echo", "agc" to "Auto gain")

/**
 * Setup: the mic (input, clean-up, meter, mic only) and the video quality. The dock in OBS has the
 * same mic controls; both send the same requests, so they always agree.
 */
@Composable
private fun SetupSheet(s: UiState, close: () -> Unit) {
    val lvl by Engine.level.collectAsState()
    val cam = s.cam
    val muted = { t: String -> @Composable { Text(t, color = Palette.muted, fontSize = 12.sp) } }
    Box(
        Modifier.fillMaxSize().background(Palette.panel)
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {},
    ) {
        Column(
            Modifier.fillMaxSize().safeDrawingPadding().verticalScroll(rememberScrollState()).padding(horizontal = 18.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Setup", color = Color.White, fontSize = 18.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
                Primary("Done") { close() }
            }
            SectionTitle("Mic")
            SheetRow(
                when { !s.micAllowed -> "Mic not allowed"; s.mic -> "Mic on"; else -> "Mic muted" },
                on = s.mic && s.micAllowed, enabled = s.micAllowed, onClick = { Engine.apply(Cmd(mic = !s.mic)) },
            ) { Meter(lvl.peak, lvl.clip, Modifier.width(180.dp)) }
            if (!s.micAllowed) muted("Allow the microphone in Android Settings → Apps → Rely-A-Cam → Permissions, then come back.")()
            s.mics.forEach { m ->
                val inUse = m.id == s.micIn
                SheetRow(m.label, on = inUse, onClick = { Engine.apply(Cmd(micIn = m.id)) }) { if (inUse) muted("in use")() }
            }
            muted("A wired, USB or Bluetooth mic shows up here as soon as it's plugged in or connected.")()
            if (s.clean.isNotEmpty()) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    s.clean.forEach { k ->
                        val on = when (k) { "ns" -> s.ns; "aec" -> s.aec; else -> s.agc }
                        Chip(CLEAN_LABEL[k] ?: k, selected = on) {
                            Engine.apply(when (k) { "ns" -> Cmd(ns = !on); "aec" -> Cmd(aec = !on); else -> Cmd(agc = !on) })
                        }
                    }
                }
                muted("Clean-up: Noise cuts steady hiss and hum, Echo removes sound from a speaker nearby, Auto gain evens out loud and quiet.")()
            }
            SheetRow("Mic only (camera off)", on = s.micOnly, onClick = { Engine.apply(Cmd(micOnly = !s.micOnly)) }) {
                muted(if (s.micOnly) "on" else "off")()
            }
            Spacer(Modifier.height(4.dp))
            SectionTitle("Video")
            if (cam != null) {
                muted("What ${cam.label} can send. Higher heats the phone and needs strong Wi-Fi.")()
                Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    cam.modes.map { it.toString() }.forEach { m ->
                        Chip(Mode.parse(m)?.label ?: m, selected = m == s.mode) { Engine.apply(Cmd(mode = m)) }
                    }
                }
                if (s.codecs.size > 1) {
                    muted("Codec")()
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        s.codecs.forEach { c -> Chip(c.uppercase(), selected = c == s.codec) { Engine.apply(Cmd(codec = c)) } }
                    }
                    muted("HEVC: same picture in less data, if the PC can decode it.")()
                }
            }
        }
    }
}

private fun nextCam(s: UiState): CameraInfo {
    val i = s.cams.indexOfFirst { it.id == s.cam?.id }
    return s.cams[(i + 1) % s.cams.size]
}

private fun statusLine(s: UiState): String = buildList {
    if (s.micOnly) add("Mic only") else {
        Mode.parse(s.mode)?.let { add(it.label) }
        add(s.codec.uppercase())
    }
    if (s.kbps > 0) add("%.1f Mb/s".format(s.kbps / 1000.0))
    if (s.temp > 0) add("%.0f°C".format(s.temp))
    if (s.battery >= 0) add("${s.battery}%")
}.joinToString(" · ")

private fun toCaps(c: CameraInfo) = CamCaps(c.id, c.facingName, c.label, c.zoomMin, c.zoomMax, c.lenses, c.flash, emptyList())

@Composable
private fun ZoomRow(cam: CamCaps, zoom: Double, pick: (Double) -> Unit) {
    val stops = Presets.of(cam)
    if (stops.isEmpty()) { Pill("This camera can't zoom"); return }
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
        stops.forEach { z -> Chip(Presets.label(z), selected = kotlin.math.abs(z - zoom) < 0.02) { pick(z) } }
    }
}

/** The ruler: a log-scale slider from the camera's own minimum to its maximum. */
@Composable
private fun ZoomSlider(cam: CameraInfo, zoom: Double) {
    val lo = ln(cam.zoomMin)
    val hi = ln(cam.zoomMax)
    val pos = ((ln(zoom) - lo) / (hi - lo)).toFloat().coerceIn(0f, 1f)
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(Presets.label(cam.zoomMin), color = Color.White, fontSize = 11.sp)
        Slider(
            value = pos, onValueChange = { Engine.apply(Cmd(zoom = exp(lo + it * (hi - lo)))) },
            modifier = Modifier.weight(1f).padding(horizontal = 8.dp),
            colors = SliderDefaults.colors(thumbColor = Color.White, activeTrackColor = Palette.blueLight, inactiveTrackColor = Color(0x66FFFFFF)),
        )
        Text(Presets.label(cam.zoomMax), color = Color.White, fontSize = 11.sp)
    }
}

/** Black-screen mode: the screen stays on but shows black, with text that drifts. */
@Composable
private fun BlackScreen(s: UiState) {
    val t = rememberInfiniteTransition(label = "drift")
    val x by t.animateFloat(0.1f, 0.6f, infiniteRepeatable(tween(23000, easing = LinearEasing), RepeatMode.Reverse), label = "x")
    val y by t.animateFloat(0.15f, 0.8f, infiniteRepeatable(tween(31000, easing = LinearEasing), RepeatMode.Reverse), label = "y")
    BoxWithConstraints(Modifier.fillMaxSize().background(Color.Black).clickable { Engine.setBlack(false) }) {
        Text(
            "Live · ${Presets.label(s.zoom)} · tap to wake",
            color = Color(0xFF3A3A3A), fontSize = 13.sp,
            modifier = Modifier.offset(maxWidth * x, maxHeight * y),
        )
    }
}

// ---- problems --------------------------------------------------------------------------------

@Composable
private fun UnreachableScreen(s: UiState) {
    val p = s.problem
    Column(
        Modifier.fillMaxSize().safeDrawingPadding().verticalScroll(rememberScrollState()).padding(24.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text("Can't reach ${p?.pairing?.pcName?.ifEmpty { null } ?: "the PC"}", color = Color.White, fontSize = 22.sp, fontWeight = FontWeight.SemiBold)
        p?.pairing?.let { Text("Tried ${it.hosts.joinToString { h -> "$h:${it.port}" }}", color = Palette.muted, fontSize = 13.sp) }
        Text(p?.message.orEmpty(), color = Palette.text)
        val fixes = buildList {
            if (p?.sameNetwork == false) {
                add("This phone and the PC are on different networks. Put both on the same Wi-Fi, or turn on this phone's hotspot and join it from the PC.")
                add("School, office and hotel Wi-Fi often stop devices from seeing each other. A phone hotspot or a USB cable gets around that.")
            }
            when (p?.reach) {
                Reach.TIMEOUT, null -> {
                    add("Windows Firewall is the usual cause, most of all on a network Windows calls Public (a hotspot is one).")
                    add("In OBS, the Phone Camera dock has an Allow phones button that fixes it. Or: Start → Allow an app through Windows Firewall → tick OBS Studio for Private and Public.")
                }
                Reach.REFUSED -> {
                    add("Open OBS on the PC. If it's already open, the Rely-A-Cam plugin isn't installed: get it from ${Updater.RELEASES}")
                }
                Reach.AUTH -> add("That PC no longer knows this phone (its pairing was reset). Scan its QR again.")
                Reach.VERSION -> add("The plugin on the PC and this app are different versions. Update both from ${Updater.RELEASES}")
                Reach.BUSY -> add("That PC is busy with another phone in the same slot. Remove it in OBS, or add this phone as a new one.")
                Reach.NO_ROUTE -> add("Turn on Wi-Fi on this phone, and check the PC is connected.")
                Reach.OTHER -> add("Try again. If it keeps failing, restart OBS.")
            }
        }
        fixes.forEach { Text("•  $it", color = Palette.text, fontSize = 14.sp) }
        Spacer(Modifier.height(8.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Primary("Retry") { Engine.retry() }
            Secondary("Scan again") { Engine.showScan() }
        }
    }
}

@Composable
private fun CenterMessage(title: String, sub: String? = null, spinner: Boolean = false, actions: @Composable () -> Unit) {
    Column(
        Modifier.fillMaxSize().safeDrawingPadding().padding(24.dp),
        verticalArrangement = Arrangement.Center, horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        if (spinner) { CircularProgressIndicator(color = Palette.blueLight); Spacer(Modifier.height(16.dp)) }
        Text(title, color = Color.White, fontSize = 18.sp, textAlign = TextAlign.Center)
        sub?.let { Spacer(Modifier.height(6.dp)); Text(it, color = Palette.muted, fontSize = 13.sp, textAlign = TextAlign.Center) }
        Spacer(Modifier.height(20.dp))
        actions()
    }
}

@Composable
private fun PermissionScreen(request: () -> Unit) {
    CenterMessage(
        "Rely-A-Cam needs the camera",
        sub = "It streams this phone's camera to OBS on your PC, directly. Nothing goes to the internet. The microphone is optional.",
    ) { Primary("Allow camera") { request() } }
}

// ---- the other half: the PC plugin -----------------------------------------------------------

/** The phone side of "each half points at the other" (Phase 4). */
@Composable
private fun GetPluginScreen() {
    val ctx = LocalContext.current
    Column(
        Modifier.fillMaxSize().safeDrawingPadding().verticalScroll(rememberScrollState()).padding(24.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text("Get the PC plugin", color = Color.White, fontSize = 22.sp, fontWeight = FontWeight.SemiBold)
        Text("Rely-A-Cam works with OBS Studio on Windows. On your PC:", color = Palette.text)
        Text("1.  Open this page and download RelyACam-Setup.exe:", color = Palette.text)
        Text(Updater.RELEASES, color = Palette.blueLight, modifier = Modifier.clickable {
            ctx.startActivity(Intent(Intent.ACTION_VIEW, android.net.Uri.parse(Updater.RELEASES)))
        })
        Text("2.  Run it. It finds OBS, installs the plugin and lets phones through Windows Firewall.", color = Palette.text)
        Text("3.  Open OBS. The Phone Camera dock shows a QR: scan it with this app.", color = Palette.text)
        Spacer(Modifier.height(4.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Primary("Send link to my PC") { share(ctx, Updater.RELEASES) }
            Secondary("Copy") {
                ctx.getSystemService(ClipboardManager::class.java)?.setPrimaryClip(ClipData.newPlainText("Rely-A-Cam", Updater.RELEASES))
            }
        }
        Secondary("Back to scanning") { Engine.showGetPlugin(false) }
    }
}

private fun share(ctx: Context, url: String) {
    val i = Intent(Intent.ACTION_SEND).setType("text/plain")
        .putExtra(Intent.EXTRA_SUBJECT, "Rely-A-Cam for OBS")
        .putExtra(Intent.EXTRA_TEXT, "Rely-A-Cam OBS plugin: $url")
    ctx.startActivity(Intent.createChooser(i, "Send the plugin link"))
}

// ---- updates and notices ---------------------------------------------------------------------

@Composable
private fun UpdateBanner(s: UiState) {
    val r = s.update ?: return
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    var progress by remember { mutableStateOf<Int?>(null) }
    var error by remember { mutableStateOf<String?>(null) }
    Row(
        Modifier.fillMaxWidth().padding(bottom = 10.dp).clip(RoundedCornerShape(6.dp)).background(Palette.blue)
            .padding(horizontal = 12.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            error ?: progress?.let { "Downloading ${r.tag}… ${if (it >= 0) "$it%" else ""}" } ?: "Rely-A-Cam ${r.tag} is out",
            color = Color.White, fontSize = 13.sp, modifier = Modifier.weight(1f),
        )
        if (progress == null) Text("Update", color = Color.White, fontWeight = FontWeight.Bold, modifier = Modifier.clickable {
            progress = 0
            error = null
            scope.launch {
                val f = withContext(Dispatchers.IO) {
                    runCatching { Updater.download(ctx, r) { p -> progress = p } }
                }
                f.onSuccess { Updater.install(ctx, it); progress = null }
                    .onFailure { error = it.message ?: "The download failed."; progress = null }
            }
        })
    }
}

@Composable
private fun Notice(text: String) {
    LaunchedEffect(text) { delay(4500); Engine.clearNotice() }
    Box(Modifier.fillMaxSize().safeDrawingPadding().padding(16.dp), contentAlignment = Alignment.BottomCenter) {
        Text(
            text, color = Color.White, fontSize = 13.sp,
            modifier = Modifier.clip(RoundedCornerShape(6.dp)).background(Palette.base).clickable { Engine.clearNotice() }
                .padding(horizontal = 14.dp, vertical = 10.dp).width(300.dp),
        )
    }
}
