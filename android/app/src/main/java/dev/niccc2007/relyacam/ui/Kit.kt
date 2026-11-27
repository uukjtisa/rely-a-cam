package dev.niccc2007.relyacam.ui

import dev.niccc2007.relyacam.protocol.GestureDet
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.drawText
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectVerticalDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.FlowRowScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Button
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathFillType
import androidx.compose.ui.graphics.Shadow
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.addPathNodes
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.layout
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.exp
import kotlin.math.floor
import kotlin.math.ln
import kotlin.math.roundToInt

/**
 * The phone app's look: camera-app dark with OBS's blue as the accent (docs/mock.html). Every
 * colour the app uses is here, so docs/DESIGN.md can point at one file.
 */
object Palette {
    val bg = Color(0xFF13141A)
    val panel = Color(0xFF1D1F26)
    val base = Color(0xFF272A33)
    val line = Color(0xFF3C404D)
    val hover = Color(0xFF464B59)
    val blue = Color(0xFF284CB8)
    val blueLight = Color(0xFF476BD7)
    val red = Color(0xFFC01C37)
    val green = Color(0xFF25A231)
    val yellow = Color(0xFFE5AF24)
    val text = Color(0xFFEDEDED)
    val muted = Color(0xFF999999)
    val scrim = Color(0x99000000)

    /** Round 5 (docs/mock.html `.rbtn`, `.pline`, `.zval`): controls floating on the picture. */
    val glass = Color(0xAA000000)
    val glassEdge = Color(0x26FFFFFF)
    /** The Setup sheet over the picture (`.psheet`), its labels and its small print. */
    val sheet = Color(0xF20B0C10)
    val sheetLabel = Color(0xFF9AA0AA)
    val sheetMuted = Color(0xFF8A8F99)
    val sheetRow = Color(0x10FFFFFF)
    /** The level line once it's level (`.levelov.ok`). */
    val level = Color(0xFF4CD964)
    val onImage = Color.White
    val shadow = Color(0xCC000000)
}

@Composable
fun RelyTheme(content: @Composable () -> Unit) = MaterialTheme(
    colorScheme = darkColorScheme(
        primary = Palette.blueLight, onPrimary = Color.White, background = Palette.bg, surface = Palette.panel,
        onSurface = Palette.text, onBackground = Palette.text, error = Palette.red, secondary = Palette.blue,
        surfaceVariant = Palette.base, outline = Palette.line,
    ),
    content = content,
)

/** A rounded label over the camera picture. */
@Composable
fun Pill(text: String, modifier: Modifier = Modifier, dot: Color? = null) {
    Row(
        modifier.clip(RoundedCornerShape(50)).background(Palette.scrim).padding(horizontal = 10.dp, vertical = 5.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        if (dot != null) Box(Modifier.size(8.dp).clip(CircleShape).background(dot))
        Text(text, color = Color.White, fontSize = 12.sp, fontWeight = FontWeight.Medium)
    }
}

/** A round control on the camera screen's rail. [on] lights it in the accent colour. */
@Composable
fun RailButton(label: String, on: Boolean = false, enabled: Boolean = true, onClick: () -> Unit) {
    Box(
        Modifier.size(52.dp).clip(CircleShape)
            .background(if (on) Palette.blue else Palette.scrim)
            .border(1.dp, if (on) Palette.blueLight else Color(0x33FFFFFF), CircleShape)
            .clickable(enabled = enabled, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            label, color = if (enabled) Color.White else Palette.muted, fontSize = 10.sp,
            textAlign = TextAlign.Center, lineHeight = 11.sp, fontWeight = FontWeight.Medium,
        )
    }
}

/** A zoom preset chip ("0.65x"). */
@Composable
fun Chip(text: String, selected: Boolean, onClick: () -> Unit) {
    Box(
        Modifier.clip(RoundedCornerShape(50))
            .background(if (selected) Color.White else Palette.scrim)
            .clickable(onClick = onClick)
            .padding(horizontal = 12.dp, vertical = 6.dp),
    ) {
        Text(text, color = if (selected) Color.Black else Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
    }
}

@Composable
fun Primary(text: String, modifier: Modifier = Modifier, onClick: () -> Unit) {
    Button(onClick = onClick, modifier = modifier, shape = RoundedCornerShape(6.dp),
        colors = ButtonDefaults.buttonColors(containerColor = Palette.blue, contentColor = Color.White)) { Text(text) }
}

@Composable
fun Secondary(text: String, modifier: Modifier = Modifier, onClick: () -> Unit) {
    OutlinedButton(onClick = onClick, modifier = modifier, shape = RoundedCornerShape(6.dp)) { Text(text, color = Palette.text) }
}

@Composable
fun Gap(w: Int) = Box(Modifier.width(w.dp))

/**
 * A mic level bar with OBS's mixer zones (green to -9 dB, yellow to -3 dB, red above), and a CLIP
 * light that is dim until the mic reaches full scale. The zones are the one place green, yellow
 * and red mean loudness instead of status (docs/DESIGN.md).
 */
@Composable
fun Meter(peak: Float, clip: Boolean, modifier: Modifier = Modifier, height: Dp = 6.dp) {
    Row(modifier, verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        Canvas(Modifier.weight(1f).height(height).clip(RoundedCornerShape(50))) {
            drawRect(Palette.line)
            val w = size.width * peak.coerceIn(0f, 1f)
            val zones = listOf(0.65f to Palette.green, 0.85f to Palette.yellow, 1f to Palette.red)
            var from = 0f
            for ((to, c) in zones) {
                val x0 = size.width * from
                val x1 = minOf(w, size.width * to)
                if (x1 > x0) drawRect(c, topLeft = Offset(x0, 0f), size = Size(x1 - x0, size.height))
                from = to
            }
        }
        Text(
            "CLIP", fontSize = 9.sp, fontWeight = FontWeight.Bold,
            color = if (clip) Color.White else Palette.muted,
            modifier = Modifier.clip(RoundedCornerShape(3.dp)).background(if (clip) Palette.red else Palette.base)
                .padding(horizontal = 4.dp, vertical = 1.dp),
        )
    }
}

// ---- round 5: the live screen's icon controls, ruler and overlays (docs/mock.html) -----------

/** The live screen's icons (docs/mock.html `IC`), on a 24-unit grid, tinted by [RoundIcon]. */
object Glyph {
    private fun stroke(name: String, vararg paths: String): ImageVector =
        ImageVector.Builder(name, 24.dp, 24.dp, 24f, 24f).apply {
            paths.forEach { d ->
                addPath(
                    addPathNodes(d), fill = null, stroke = SolidColor(Color.White), strokeLineWidth = 2f,
                    strokeLineCap = StrokeCap.Round, strokeLineJoin = StrokeJoin.Round,
                )
            }
        }.build()

    private fun filled(name: String, d: String): ImageVector =
        ImageVector.Builder(name, 24.dp, 24.dp, 24f, 24f)
            .addPath(addPathNodes(d), pathFillType = PathFillType.EvenOdd, fill = SolidColor(Color.White))
            .build()

    private const val MIC_BODY = "M12 3a3 3 0 0 1 3 3v6a3 3 0 0 1-6 0V6a3 3 0 0 1 3-3z"

    val torch = stroke("torch", "M7 2h10l-2 6H9z", "M9 8h6v12a2 2 0 0 1-2 2h-2a2 2 0 0 1-2-2z", "M12 12v3")
    val flip = stroke("flip", "M4 10a8 8 0 0 1 14-4l2 2", "M20 3v5h-5", "M20 14a8 8 0 0 1-14 4l-2-2", "M4 21v-5h5")
    val mic = stroke("mic", MIC_BODY, "M5 11a7 7 0 0 0 14 0M12 18v3")
    val micOff = stroke("micOff", MIC_BODY, "M5 11a7 7 0 0 0 14 0M12 18v3", "M3 3l18 18")
    val moon = stroke("moon", "M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5z")
    val gear = filled(
        "gear",
        "M10.3 2h3.4l.5 2.6a7.9 7.9 0 0 1 1.9 1.1l2.5-.9 1.7 2.9-2 1.7a8 8 0 0 1 0 2.2l2 1.7-1.7 2.9-2.5-.9" +
            "a7.9 7.9 0 0 1-1.9 1.1l-.5 2.6h-3.4l-.5-2.6a7.9 7.9 0 0 1-1.9-1.1l-2.5.9-1.7-2.9 2-1.7a8 8 0 0 1 0-2.2" +
            "l-2-1.7 1.7-2.9 2.5.9a7.9 7.9 0 0 1 1.9-1.1zM12 8.6a3.4 3.4 0 1 0 0 6.8 3.4 3.4 0 0 0 0-6.8z",
    )
}

/**
 * A round icon control floating on the picture (`.rbtn`): glass when off, white with a black icon
 * when on. [name] is its accessibility label and what a long press shows ([onHint]).
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
fun RoundIcon(
    icon: ImageVector,
    name: String,
    on: Boolean = false,
    size: Dp = 48.dp,
    onHint: ((String) -> Unit)? = null,
    onClick: () -> Unit,
) {
    Box(
        Modifier.size(size).clip(CircleShape)
            .background(if (on) Palette.onImage else Palette.glass)
            .border(1.dp, Palette.glassEdge, CircleShape)
            .combinedClickable(onClickLabel = name, onLongClick = onHint?.let { { it(name) } }, onClick = onClick)
            .semantics { contentDescription = name },
        contentAlignment = Alignment.Center,
    ) {
        Icon(icon, contentDescription = null, tint = if (on) Color.Black else Palette.onImage, modifier = Modifier.size(size / 2))
    }
}

/**
 * The one status line on the live screen (`.pline`): a dot, then "Live · Wi-Fi · 1080p30 · 41°C".
 * With [hot] set, the line turns yellow and ends with it in bold.
 */
@Composable
fun StatusLine(text: String, modifier: Modifier = Modifier, dot: Color = Palette.red, hot: String? = null, onClick: () -> Unit) {
    Row(
        modifier.clip(RoundedCornerShape(14.dp)).background(Palette.glass).clickable(onClick = onClick)
            .padding(horizontal = 10.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Box(Modifier.size(7.dp).shadow(4.dp, CircleShape, ambientColor = dot, spotColor = dot).clip(CircleShape).background(dot))
        Text(
            buildAnnotatedString {
                append(text)
                if (hot != null) { append(" · "); withStyle(SpanStyle(fontWeight = FontWeight.Bold)) { append(hot) } }
            },
            color = if (hot != null) Palette.yellow else Palette.onImage, fontSize = 12.sp,
        )
    }
}

/** The zoom value over the ruler (`.zval`). */
@Composable
fun ZoomValue(text: String) {
    Text(
        text, color = Palette.onImage, fontSize = 14.sp, fontWeight = FontWeight.SemiBold,
        style = TextStyle(fontFeatureSettings = "tnum"),
        modifier = Modifier.clip(RoundedCornerShape(12.dp)).background(Palette.glass).padding(horizontal = 10.dp, vertical = 2.dp),
    )
}

private val ImageText = TextStyle(
    color = Color.White, fontSize = 11.sp, fontWeight = FontWeight.SemiBold,
    shadow = Shadow(Color.Black, Offset(0f, 2f), 4f),
)

/**
 * The slim zoom ruler (`.sruler`), shared look with the dock's: a log-scale track from the camera's
 * own minimum to its maximum, a labelled tick at every preset and at the maximum, small ticks at
 * every whole zoom between, and a white handle at [zoom]. Drag anywhere on it; a tap on a label
 * goes to that value.
 */
@Composable
fun SlimRuler(
    min: Double, max: Double, labels: List<Double>, zoom: Double, label: (Double) -> String,
    modifier: Modifier = Modifier, onZoom: (Double) -> Unit,
) {
    val lo = ln(min)
    val hi = ln(max)
    fun pos(v: Double) = if (hi <= lo) 0f else ((ln(v) - lo) / (hi - lo)).toFloat().coerceIn(0f, 1f)
    fun value(p: Float) = exp(lo + p.coerceIn(0f, 1f) * (hi - lo))
    val minors = (ceil(min).toInt()..floor(max).toInt()).map { it.toDouble() }
        .filter { v -> labels.none { abs(it - v) < 1e-6 } }
    val density = LocalDensity.current
    val snapPx = with(density) { 18.dp.toPx() }
    Box(
        modifier.fillMaxWidth().height(36.dp).padding(horizontal = 16.dp)
            .pointerInput(min, max, labels) {
                detectTapGestures { o ->
                    val w = size.width.toFloat()
                    val near = labels.minByOrNull { abs(pos(it) * w - o.x) }?.takeIf { abs(pos(it) * w - o.x) < snapPx }
                    onZoom(near ?: value(o.x / w))
                }
            }
            .pointerInput(min, max) {
                detectHorizontalDragGestures(onDragStart = { onZoom(value(it.x / size.width)) }) { ch, _ ->
                    ch.consume()
                    onZoom(value(ch.position.x / size.width))
                }
            },
    ) {
        Canvas(Modifier.fillMaxSize()) {
            val cy = 10.dp.toPx()
            val white = Color.White
            drawLine(Color(0x70FFFFFF), Offset(0f, cy), Offset(size.width, cy), 2.dp.toPx(), StrokeCap.Round)
            minors.forEach { v ->
                val x = pos(v) * size.width
                drawLine(Color(0xA0FFFFFF), Offset(x, cy - 4.dp.toPx()), Offset(x, cy + 4.dp.toPx()), 1.dp.toPx())
            }
            labels.forEach { v ->
                val x = pos(v) * size.width
                drawLine(white, Offset(x, cy - 7.dp.toPx()), Offset(x, cy + 7.dp.toPx()), 2.dp.toPx())
            }
            val hx = pos(zoom) * size.width
            drawCircle(Palette.shadow.copy(alpha = 0.45f), 10.dp.toPx(), Offset(hx, cy + 1.dp.toPx()))
            drawCircle(white, 8.dp.toPx(), Offset(hx, cy))
        }
        labels.forEach { v ->
            Text(
                label(v), style = ImageText,
                modifier = Modifier.layout { m, c ->
                    val p = m.measure(c.copy(minWidth = 0, minHeight = 0))
                    layout(0, 0) { p.place((pos(v) * c.maxWidth - p.width / 2f).roundToInt(), 19.dp.roundToPx()) }
                },
            )
        }
    }
}

/** The rule-of-thirds grid (`.gridov`): two lines each way, white with a faint dark edge. */
@Composable
fun GridOverlay(modifier: Modifier = Modifier) {
    Canvas(modifier.fillMaxSize()) {
        val edge = Color(0x59000000)
        val line = Color(0x8CFFFFFF)
        for (f in listOf(1f / 3, 2f / 3)) {
            val x = size.width * f
            val y = size.height * f
            drawLine(edge, Offset(x, 0f), Offset(x, size.height), 3f)
            drawLine(edge, Offset(0f, y), Offset(size.width, y), 3f)
            drawLine(line, Offset(x, 0f), Offset(x, size.height), 1.dp.toPx())
            drawLine(line, Offset(0f, y), Offset(size.width, y), 1.dp.toPx())
        }
    }
}

/**
 * The gestures overlay, in the gestures app's own colours so the phone, the dock and that app's
 * window look the same (docs/DESIGN.md rule 4, the one exception): a note-green skeleton with white
 * joints, a ring that fills while a gesture is held (amber while it waits for the confirm sign),
 * and its name on a dark pill under it. [mapper] is given the canvas size and returns where a
 * point of the upright picture (0..1) lands on screen.
 */
@Composable
fun GestureMarks(det: GestureDet, modifier: Modifier = Modifier, mapper: (w: Float, h: Float) -> (Float, Float) -> Offset) {
    val measurer = rememberTextMeasurer()
    Canvas(modifier.fillMaxSize()) {
        val at = mapper(size.width, size.height)
        val u = density
        // The face map's lines: the jaw, brows, eyes, lips and nose.
        for (l in det.face) {
            if (l.size < 4) continue
            val path = Path()
            val first = at(l[0], l[1])
            path.moveTo(first.x, first.y)
            var i = 2
            while (i + 1 < l.size) {
                val pt = at(l[i], l[i + 1])
                path.lineTo(pt.x, pt.y)
                i += 2
            }
            drawPath(path, GESTURE_SKELETON, style = Stroke(width = 1.6f * u, cap = StrokeCap.Round, join = StrokeJoin.Round))
        }
        for (hand in det.hands) {
            if (hand.size < 42) continue
            val pts = List(21) { i -> at(hand[2 * i], hand[2 * i + 1]) }
            for ((a, b) in HAND_BONES) drawLine(GESTURE_SKELETON, pts[a], pts[b], strokeWidth = 2.5f * u, cap = StrokeCap.Round)
            for (i in 1 until 21) drawCircle(Color.White, radius = 2.2f * u, center = pts[i])
            drawCircle(GESTURE_SKELETON, radius = 3.5f * u, center = pts[0])
        }
        for (l in det.labels) {
            val waiting = l.state == "waiting"
            val c = at(l.x, l.y) + Offset(0f, 30 * u)
            val r = 19 * u
            val ring = Stroke(width = 5 * u, cap = StrokeCap.Round)
            drawCircle(GESTURE_RING_TRACK, radius = r, center = c, style = ring)
            drawArc(
                if (waiting) Palette.yellow else GESTURE_SKELETON, startAngle = -90f,
                sweepAngle = 360f * (if (waiting) 1f else l.armed.coerceIn(0f, 1f)), useCenter = false,
                topLeft = c - Offset(r, r), size = Size(2 * r, 2 * r), style = ring,
            )
            val text = if (waiting && l.confirm.isNotEmpty()) "${l.name}: show ${l.confirm} with your other hand" else l.name
            val laid = measurer.measure(
                text, TextStyle(color = if (waiting) Palette.yellow else Color.White, fontSize = 12.sp, fontWeight = FontWeight.SemiBold),
            )
            val w = laid.size.width + 16 * u
            val h = laid.size.height + 4 * u
            val tl = Offset(c.x - w / 2, c.y + 26 * u)
            drawRoundRect(GESTURE_PILL, topLeft = tl, size = Size(w, h), cornerRadius = CornerRadius(h / 2, h / 2))
            drawText(laid, topLeft = Offset(tl.x + 8 * u, tl.y + 2 * u))
        }
    }
}

private val GESTURE_SKELETON = Color(0xFF5FD06C)
private val GESTURE_RING_TRACK = Color(0x88000000)
private val GESTURE_PILL = Color(0xB8000000)

/** MediaPipe's hand skeleton: the bones between its 21 points. */
private val HAND_BONES = listOf(
    0 to 1, 1 to 2, 2 to 3, 3 to 4, 0 to 5, 5 to 6, 6 to 7, 7 to 8, 5 to 9, 9 to 10, 10 to 11, 11 to 12,
    9 to 13, 13 to 14, 14 to 15, 15 to 16, 13 to 17, 0 to 17, 17 to 18, 18 to 19, 19 to 20,
)

/**
 * The level line (`.levelov`): a short line across the middle that stays parallel to the real
 * horizon, yellow with its angle until it's within half a degree, then green with "level".
 * [angle] is the screen's tilt in degrees, clockwise positive; null hides it (phone lying flat).
 */
@Composable
fun LevelLine(angle: Float?, modifier: Modifier = Modifier) {
    if (angle == null) return
    val ok = abs(angle) < 0.5f
    val c = if (ok) Palette.level else Palette.yellow
    val text = if (ok) "level" else (if (angle > 0) "+" else "") + "%.1f°".format(angle)
    BoxWithConstraints(modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        val lineW = maxWidth * 0.36f
        Box(Modifier.width(lineW + 120.dp).rotate(angle), contentAlignment = Alignment.Center) {
            Box(Modifier.width(lineW).height(2.dp).background(c))
            Text(text, style = ImageText.copy(color = c), modifier = Modifier.align(Alignment.CenterEnd).width(54.dp))
        }
    }
}

/** "AE/AF LOCK" (`.lockbadge`): focus and exposure are locked. */
@Composable
fun LockBadge(modifier: Modifier = Modifier) {
    Text(
        "AE/AF LOCK", color = Color.Black, fontSize = 11.sp, fontWeight = FontWeight.Bold,
        modifier = modifier.clip(RoundedCornerShape(10.dp)).background(Palette.yellow).padding(horizontal = 8.dp, vertical = 3.dp),
    )
}

/** Where focus is (`.fring`): a ring, yellow while locked. */
@Composable
fun FocusMark(at: Offset, locked: Boolean) {
    Canvas(Modifier.fillMaxSize()) {
        drawCircle(if (locked) Palette.yellow else Color.White, 28.dp.toPx(), at, style = Stroke(2.dp.toPx()))
    }
}

/**
 * Brightness beside the focus ring (`.evs`): a sun over a short vertical slider, in the camera's own
 * steps from [min] (bottom) to [max] (top). [onTouch] fires on every touch, so the caller can keep
 * it on screen while it's used.
 */
@Composable
fun EvSlider(index: Int, min: Int, max: Int, modifier: Modifier = Modifier, onTouch: () -> Unit, onIndex: (Int) -> Unit) {
    fun pick(y: Float, h: Float): Int = (min + (1f - (y / h).coerceIn(0f, 1f)) * (max - min)).roundToInt()
    Column(modifier, horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(2.dp)) {
        Text("☀", style = ImageText.copy(fontSize = 14.sp))
        Canvas(
            Modifier.width(28.dp).height(84.dp)
                .pointerInput(min, max) {
                    detectTapGestures(onPress = { onTouch() }) { onIndex(pick(it.y, size.height.toFloat())) }
                }
                .pointerInput(min, max) {
                    detectVerticalDragGestures(onDragStart = { onTouch() }) { ch, _ ->
                        ch.consume()
                        onTouch()
                        onIndex(pick(ch.position.y, size.height.toFloat()))
                    }
                },
        ) {
            val x = size.width / 2
            val pad = 7.dp.toPx()
            drawLine(Color(0x99FFFFFF), Offset(x, pad), Offset(x, size.height - pad), 2.dp.toPx(), StrokeCap.Round)
            val f = if (max > min) (index - min).toFloat() / (max - min) else 0.5f
            val y = pad + (1f - f) * (size.height - 2 * pad)
            drawCircle(Palette.shadow.copy(alpha = 0.45f), 8.dp.toPx(), Offset(x, y + 1.dp.toPx()))
            drawCircle(Color.White, 7.dp.toPx(), Offset(x, y))
        }
    }
}

/** Stop (`.stopbtn`): the one red button. */
@Composable
fun StopButton(modifier: Modifier = Modifier, onClick: () -> Unit) {
    Text(
        "Stop", color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold,
        modifier = modifier.clip(RoundedCornerShape(16.dp)).background(Palette.red).clickable(onClick = onClick)
            .padding(horizontal = 14.dp, vertical = 9.dp),
    )
}

/** A pick inside the Setup sheet (`.psheet .pchip`): plain text, white and bold when chosen. */
@Composable
fun SheetChip(text: String, on: Boolean, onClick: () -> Unit) {
    Box(
        Modifier.heightIn(min = 32.dp).widthIn(min = 44.dp).clip(RoundedCornerShape(16.dp))
            .background(if (on) Color.White else Color.Transparent).clickable(onClick = onClick)
            .padding(horizontal = 8.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(text, color = if (on) Color.Black else Palette.onImage, fontSize = 13.sp,
            fontWeight = if (on) FontWeight.SemiBold else FontWeight.Normal)
    }
}

/** A labelled row of [SheetChip]s (`.chipset`); the label column is the same width in every row. */
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun ChipSet(label: String? = null, content: @Composable FlowRowScope.() -> Unit) {
    FlowRow(verticalArrangement = Arrangement.Center, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        if (label != null) {
            Text(label, color = Palette.sheetLabel, fontSize = 13.sp, modifier = Modifier.widthIn(min = 100.dp).align(Alignment.CenterVertically))
        }
        content()
    }
}

/** Small print in the Setup sheet (`.psheet .muted`, `.info`). */
@Composable
fun SheetNote(text: String) = Text(text, color = Palette.sheetMuted, fontSize = 12.sp)

/** A heading inside a sheet ("MIC", "VIDEO"). */
@Composable
fun SectionTitle(text: String) {
    Text(text.uppercase(), color = Palette.sheetLabel, fontSize = 12.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 0.7.sp)
}

/** One choice in a sheet: a row that shows [on] with the accent outline, with optional trailing content. */
@Composable
fun SheetRow(
    text: String,
    on: Boolean,
    enabled: Boolean = true,
    onClick: () -> Unit,
    trailing: @Composable () -> Unit = {},
) {
    Row(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(8.dp)).background(Palette.sheetRow)
            .border(1.dp, if (on) Palette.blueLight else Color.Transparent, RoundedCornerShape(8.dp))
            .clickable(enabled = enabled, onClick = onClick).padding(horizontal = 12.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text(text, color = if (enabled) Palette.text else Palette.muted, fontSize = 14.sp, modifier = Modifier.weight(1f))
        trailing()
    }
}
