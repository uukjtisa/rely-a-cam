package dev.niccc2007.relyacam.ui

import org.json.JSONObject
import dev.niccc2007.relyacam.protocol.GestureLib
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.compose.rememberLauncherForActivityResult
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
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import android.os.SystemClock
import androidx.compose.foundation.layout.PaddingValues
import dev.niccc2007.relyacam.Engine
import dev.niccc2007.relyacam.Screen
import dev.niccc2007.relyacam.UiState
import dev.niccc2007.relyacam.camera.CameraInfo
import dev.niccc2007.relyacam.protocol.CamCaps
import dev.niccc2007.relyacam.protocol.Cmd
import dev.niccc2007.relyacam.protocol.Heat
import dev.niccc2007.relyacam.protocol.Mode
import dev.niccc2007.relyacam.protocol.Presets
import dev.niccc2007.relyacam.protocol.WhiteBalance
import dev.niccc2007.relyacam.stream.Reach
import dev.niccc2007.relyacam.update.Updater
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.math.roundToInt

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

/** How long the focus ring and the brightness slider stay after a tap (docs/mock.html, 3 s). */
private const val FOCUS_UI_MS = 3000L

/**
 * The live screen (docs/mock.html round 5, `phoneLiveHTML`): the picture full-bleed, one status
 * line, the slim ruler, and a handful of icon buttons. Landscape puts them on a rail at the right
 * with Stop at the bottom left; portrait puts Stop and the buttons in one bar at the bottom. The
 * rest lives in Setup.
 */
@Composable
private fun LiveScreen(s: UiState) {
    var setup by remember { mutableStateOf(false) }
    var library by remember { mutableStateOf(false) }
    val cam = s.cam ?: return
    // The last tap, and when the ring or slider was last touched: both show for 3 s after it, and
    // for as long as focus is locked (a lock from the PC has no point, so it shows in the middle).
    var focusAt by remember(cam.id) { mutableStateOf<Offset?>(null) }
    var touchedAt by remember { mutableStateOf(0L) }
    LaunchedEffect(touchedAt, s.aeLock) {
        if (!s.aeLock && focusAt != null) {
            delay(FOCUS_UI_MS)
            if (!Engine.ui.value.aeLock) focusAt = null
        }
    }
    val touch = { touchedAt = SystemClock.uptimeMillis() }
    val hint = { t: String -> Engine.hint(t) }
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val portrait = maxHeight > maxWidth
        CameraView(s, applyMirror = true, onFocus = { at, _ -> focusAt = at; touch() })
        if (s.micOnly) MicOnlyScreen(s) else {
            GestureOverlay(s)
            if (s.grid.phone) GridOverlay()
            if (s.levelLine) Level()
            val ring = focusAt ?: if (s.aeLock) Offset(constraints.maxWidth / 2f, constraints.maxHeight / 2f) else null
            if (ring != null) {
                FocusMark(ring, s.aeLock)
                if (cam.hasEv) {
                    val d = LocalDensity.current
                    val w = constraints.maxWidth
                    EvSlider(
                        s.evIndex, cam.evMin, cam.evMax,
                        Modifier.offset {
                            val right = ring.x + 34.dp.toPx()
                            val x = if (right + 28.dp.toPx() < w) right else ring.x - 62.dp.toPx()
                            IntOffset(x.roundToInt(), (ring.y - with(d) { 52.dp.toPx() }).roundToInt())
                        },
                        onTouch = touch,
                    ) { i -> if (i != s.evIndex) Engine.apply(Cmd(ev = i * cam.evStep)) }
                }
            }
        }
        Box(Modifier.fillMaxSize().safeDrawingPadding()) {
            if (s.aeLock && !s.micOnly) LockBadge(Modifier.align(Alignment.TopCenter).padding(top = 44.dp))
            StatusLine(
                liveLine(s),
                Modifier.align(Alignment.TopStart).padding(start = 12.dp, top = 10.dp, end = if (portrait) 12.dp else 120.dp),
                dot = if (s.reconnecting) Palette.yellow else Palette.red,
                hot = if (Heat.hot(s.temp) && !s.micOnly) heatHint(cam, s.mode) else null,
            ) { setup = true }
            val buttons = @Composable { size: Dp ->
                // With the camera off, only what still works stays (DESIGN.md rule 7).
                if (!s.micOnly && s.cams.size > 1) RoundIcon(Glyph.flip, "Camera: ${nextCam(s).label}", size = size, onHint = hint) {
                    Engine.apply(Cmd(cam = nextCam(s).id))
                }
                if (s.micAllowed) RoundIcon(if (s.mic) Glyph.mic else Glyph.micOff, if (s.mic) "Mic" else "Mic muted", on = !s.mic, size = size, onHint = hint) {
                    Engine.apply(Cmd(mic = !s.mic))
                }
                if (!s.micOnly && cam.flash) RoundIcon(Glyph.torch, "Torch", on = s.torch, size = size, onHint = hint) {
                    Engine.apply(Cmd(torch = !s.torch))
                }
                RoundIcon(Glyph.gear, "Setup", size = size, onHint = hint) { setup = true }
                RoundIcon(Glyph.moon, "Dark screen", size = size, onHint = hint) { Engine.setBlack(true) }
            }
            if (portrait) {
                Row(
                    Modifier.align(Alignment.BottomCenter).padding(bottom = 10.dp),
                    horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically,
                ) {
                    StopButton { Engine.stop() }
                    buttons(40.dp)
                }
            } else {
                Column(
                    Modifier.align(Alignment.CenterEnd).padding(end = 10.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                ) { buttons(48.dp) }
                StopButton(Modifier.align(Alignment.BottomStart).padding(12.dp)) { Engine.stop() }
            }
            if (!s.micOnly) Column(
                Modifier.align(Alignment.BottomCenter).fillMaxWidth()
                    .padding(if (portrait) PaddingValues(start = 8.dp, end = 8.dp, bottom = 60.dp) else PaddingValues(start = 96.dp, end = 70.dp, bottom = 6.dp)),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(2.dp),
            ) {
                if (cam.zoomMax > cam.zoomMin + 1e-3) {
                    ZoomValue(Presets.label(s.zoom))
                    val caps = toCaps(cam)
                    SlimRuler(cam.zoomMin, cam.zoomMax, (Presets.of(caps) + cam.zoomMax).distinct(), s.zoom, Presets::label) {
                        Engine.apply(Cmd(zoom = it))
                    }
                } else {
                    Pill("${cam.label}: this camera can't zoom")
                }
            }
        }
        if (setup) SetupSheet(s, openLibrary = { setup = false; library = true }) { setup = false }
        if (library && s.gestures != null) GesturesSheet(s.gestures) { library = false }
        if (s.black) BlackScreen(s)
    }
}

/** The level line, turned to the screen: the phone's roll minus the display's own rotation. */
@Composable
private fun Level() {
    val tilt by Engine.tilt.collectAsState()
    LocalConfiguration.current // a turn of the phone changes the display rotation
    val turned = (LocalView.current.display?.rotation ?: 0) * 90
    LevelLine(tilt?.let { ((it - turned + 540f) % 360f) - 180f })
}

/** "Live · Wi-Fi · 1080p30 · 41°C", or the reconnecting state. */
private fun liveLine(s: UiState): String = buildList {
    if (s.reconnecting) add("Reconnecting to ${s.pcName.ifEmpty { "the PC" }}…") else {
        add("Live")
        if (s.transport.isNotEmpty()) add(s.transport)
    }
    if (s.micOnly) add("Mic only") else Mode.parse(s.mode)?.let { add(it.label.replace(" ", "")) }
    if (s.temp > 0) add("%.0f°C".format(s.temp))
}.joinToString(" · ")

/** "hot: try 1080p" (PROTOCOL.md, Heat). The rate is named only when it differs from now. */
private fun heatHint(cam: CameraInfo, mode: String): String {
    val next = Heat.suggest(cam.modes.map { it.toString() }, mode)?.let { Mode.parse(it) } ?: return "hot"
    val cur = Mode.parse(mode)
    val label = if (cur != null && cur.fps == next.fps) next.label.substringBefore(" ") else next.label.replace(" ", "")
    return "hot: try $label"
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
 * Setup (docs/mock.html round 5, `setupSheetHTML`), over the picture: Mic, Video, and while the
 * camera is on, Picture and Overlays. The dock in OBS reaches the same settings through the same
 * requests, so both always agree. An option that depends on another is hidden while that one is
 * off (the mic's inputs while it's muted, Picture while the camera is off).
 */
@Composable
private fun SetupSheet(s: UiState, openLibrary: () -> Unit, close: () -> Unit) {
    val lvl by Engine.level.collectAsState()
    val cam = s.cam
    Box(
        Modifier.fillMaxSize().background(Palette.sheet)
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {},
    ) {
        Column(
            Modifier.fillMaxSize().safeDrawingPadding().verticalScroll(rememberScrollState()).padding(horizontal = 18.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Setup", color = Color.White, fontSize = 16.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
                Primary("Done") { close() }
            }

            // Gestures, when the PC has Rely-A-Cam-Gestures (the plugin says so in WELCOME).
            s.gestures?.let { g ->
                SectionTitle("Gestures")
                SheetRow("Gestures on for this phone", on = g.on, onClick = { Engine.setGesturesOn(!g.on) }) {
                    SheetNote(if (g.on) "on" else "off")
                }
                if (g.on && g.status.isNotEmpty()) SheetNote(g.status)
                SheetRow("Library: gestures and memes", on = false, onClick = openLibrary) {
                    SheetNote(if (g.linked) "${g.gestures.count { it.on }} on · ${g.memes.size} memes" else "›")
                }
            }

            SectionTitle("Mic")
            SheetRow(
                when { !s.micAllowed -> "Mic not allowed"; s.mic -> "Mic on"; else -> "Mic muted" },
                on = s.mic && s.micAllowed, enabled = s.micAllowed, onClick = { Engine.apply(Cmd(mic = !s.mic)) },
            ) { if (s.mic && s.micAllowed) Meter(lvl.peak, lvl.clip, Modifier.fillMaxWidth(0.45f)) }
            if (!s.micAllowed) SheetNote("Allow the microphone in Android Settings → Apps → Rely-A-Cam → Permissions, then come back.")
            if (s.mic && s.micAllowed) {
                s.mics.forEach { m ->
                    val inUse = m.id == s.micIn
                    SheetRow(m.label, on = inUse, onClick = { Engine.apply(Cmd(micIn = m.id)) }) { if (inUse) SheetNote("in use") }
                }
                SheetNote("A wired, USB or Bluetooth mic shows up here as soon as it's plugged in or connected.")
                if (s.clean.isNotEmpty()) ChipSet {
                    s.clean.forEach { k ->
                        val on = when (k) { "ns" -> s.ns; "aec" -> s.aec; else -> s.agc }
                        SheetChip(CLEAN_LABEL[k] ?: k, on) {
                            Engine.apply(when (k) { "ns" -> Cmd(ns = !on); "aec" -> Cmd(aec = !on); else -> Cmd(agc = !on) })
                        }
                    }
                }
            }
            SheetRow("Mic only (camera off)", on = s.micOnly, onClick = { Engine.apply(Cmd(micOnly = !s.micOnly)) }) {
                SheetNote(if (s.micOnly) "on" else "off")
            }

            SectionTitle("Video")
            if (cam != null) {
                ChipSet {
                    cam.modes.map { it.toString() }.forEach { m ->
                        SheetChip(Mode.parse(m)?.label?.replace(" ", "") ?: m, m == s.mode) { Engine.apply(Cmd(mode = m)) }
                    }
                }
                if (s.codecs.size > 1) ChipSet("Codec") {
                    s.codecs.forEach { c -> SheetChip(c.uppercase(), c == s.codec) { Engine.apply(Cmd(codec = c)) } }
                }
                SheetNote(videoInfo(s))
            }

            if (cam != null && !s.micOnly) {
                SectionTitle("Picture")
                ChipSet {
                    SheetChip("Mirror", s.mirror) { Engine.apply(Cmd(mirror = !s.mirror)) }
                    SheetChip("Flip", s.flip) { Engine.apply(Cmd(flip = !s.flip)) }
                    SheetChip("Lock turn", s.lock) { Engine.apply(Cmd(lock = !s.lock)) }
                }
                if (cam.whiteBalances.size > 1) ChipSet("White balance") {
                    cam.whiteBalances.forEach { k -> SheetChip(WhiteBalance.label(k), k == s.wb) { Engine.apply(Cmd(wb = k)) } }
                }
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    if (s.snap.isNotEmpty()) Secondary("Snapshot to PC") { Engine.apply(Cmd(snap = true)) }
                    SheetNote(
                        (if (s.snap.isEmpty()) "No snapshots at this quality on this camera. " else "") +
                            if (cam.aeLock) "Tap the picture to focus, hold to lock." else "Tap the picture to focus.",
                    )
                }

                SectionTitle("Overlays")
                ChipSet("Grid") {
                    GRID_PLACES.forEach { (k, label) ->
                        val on = when (k) { "dock" -> s.grid.dock; "phone" -> s.grid.phone; else -> s.grid.out }
                        SheetChip(label, on) { Engine.apply(Cmd(grid = mapOf(k to !on))) }
                    }
                }
                ChipSet("Level line") {
                    SheetChip(if (s.levelLine) "On" else "Off", s.levelLine) { Engine.setLevelLine(!s.levelLine) }
                    Box(Modifier.align(Alignment.CenterVertically).padding(start = 6.dp)) { SheetNote("this phone only") }
                }
            }
        }
    }
}

private val GRID_PLACES = listOf("dock" to "Dock", "phone" to "Phone", "out" to "OBS output")

private fun nextCam(s: UiState): CameraInfo {
    val i = s.cams.indexOfFirst { it.id == s.cam?.id }
    return s.cams[(i + 1) % s.cams.size]
}

/** Setup's info line: "H.264 · 31.5 Mb/s · battery 78% · 41°C" (moved off the live screen). */
private fun videoInfo(s: UiState): String = buildList {
    add(if (s.codec == "hevc") "HEVC" else "H.264")
    if (s.kbps > 0) add("%.1f Mb/s".format(s.kbps / 1000.0))
    if (s.battery >= 0) add("battery ${s.battery}%")
    if (s.temp > 0) add("%.0f°C".format(s.temp))
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


/**
 * The gestures Library (Rely-A-Cam-Gestures on the PC, through the plugin): which gestures are on,
 * where each meme shows, memes added from this phone's gallery, and a take recorded on the PC.
 * Every edit is done on the PC, which sends the library back, so both screens always agree.
 */
@Composable
private fun GesturesSheet(g: GestureLib, close: () -> Unit) {
    var tab by remember { mutableStateOf(0) }
    val pick = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) Engine.uploadMeme(uri)
    }
    Box(
        Modifier.fillMaxSize().background(Palette.sheet)
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null) {},
    ) {
        Column(
            Modifier.fillMaxSize().safeDrawingPadding().verticalScroll(rememberScrollState()).padding(horizontal = 18.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Gestures", color = Color.White, fontSize = 16.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.weight(1f))
                Primary("Done") { close() }
            }
            SheetRow("Gestures on for this phone", on = g.on, onClick = { Engine.setGesturesOn(!g.on) }) {
                SheetNote(if (g.on) "on" else "off")
            }
            if (g.status.isNotEmpty()) SheetNote(g.status)
            if (!g.linked) {
                SheetNote("The library shows here once the gestures app on the PC is running (turn gestures on).")
            } else {
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    SheetChip("Controls", tab == 0) { tab = 0 }
                    SheetChip("Reactions", tab == 1) { tab = 1 }
                    SheetChip("Memes", tab == 2) { tab = 2 }
                }
                when (tab) {
                    0 -> ControlList(g)
                    1 -> GestureList(g)
                    else -> MemeList(g) { pick.launch(arrayOf("image/*", "video/*")) }
                }
            }
        }
    }
}

/** The PC's Controls tab: each job on or off here; its gesture is picked on the PC. */
@Composable
private fun ControlList(g: GestureLib) {
    var last = ""
    g.controls.forEach { c ->
        if (c.group != last) {
            last = c.group
            SheetNote(c.group)
        }
        SheetRow(c.label, on = c.on, onClick = {
            Engine.gestureEdit(JSONObject().put("control", JSONObject().put("id", c.id).put("on", !c.on)))
        }) {
            SheetNote(if (c.move) "pinch" else c.gesture.ifEmpty { "no gesture" })
        }
    }
    if (g.controls.isEmpty()) SheetNote("Update the gestures app on the PC to see its controls.")
    SheetNote("Pinch zoom: pinch and hold still, push toward the camera to zoom in, pull back to zoom out.")
}

@Composable
private fun GestureList(g: GestureLib) {
    g.gestures.forEach { item ->
        SheetRow(item.name, on = item.on, onClick = {
            // A control's gesture is switched with its control, on the Controls tab.
            if (item.control.isEmpty()) {
                Engine.gestureEdit(JSONObject().put("gesture", JSONObject().put("id", item.id).put("on", !item.on)))
            }
        }) {
            SheetNote(if (item.control.isNotEmpty()) "runs ${item.control}" else item.action.ifEmpty { "no meme yet" })
            if (item.mine) SheetChip("Record", false) { Engine.gestureEdit(JSONObject().put("record", item.id)) }
        }
    }
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Secondary("New gesture") { Engine.gestureEdit(JSONObject().put("newGesture", true)) }
        Secondary("Open the editor on the PC") { Engine.gestureEdit(JSONObject().put("openEditor", true)) }
    }
    SheetNote("A new gesture or a take is recorded on the PC from this camera: 3, 2, 1, then hold it for 2 s.")
}

@Composable
private fun MemeList(g: GestureLib, add: () -> Unit) {
    g.memes.forEach { m ->
        Column(
            Modifier.fillMaxWidth().clip(RoundedCornerShape(8.dp)).background(Palette.sheetRow).padding(horizontal = 12.dp, vertical = 8.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(m.name, color = Palette.text, fontSize = 14.sp, modifier = Modifier.weight(1f))
                SheetNote(m.kind + if (m.sound) " · sound" else "")
            }
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp), modifier = Modifier.horizontalScroll(rememberScrollState())) {
                MEME_PLACES.forEach { (k, label) ->
                    SheetChip(label, m.place == k) {
                        Engine.gestureEdit(JSONObject().put("meme", JSONObject().put("id", m.id).put("place", k)))
                    }
                }
                SheetChip("Remove", false) { Engine.gestureEdit(JSONObject().put("removeMeme", m.id)) }
            }
        }
    }
    if (g.memes.isEmpty()) SheetNote("No memes yet.")
    Primary("Add from this phone") { add() }
    SheetNote("Pictures, GIFs and videos (WebM keeps its transparency), up to 15 MB.")
}

private val MEME_PLACES = listOf("head" to "On my head", "full" to "Full frame", "spot" to "Its spot")
