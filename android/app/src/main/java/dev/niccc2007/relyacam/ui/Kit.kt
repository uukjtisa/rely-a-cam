package dev.niccc2007.relyacam.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Button
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

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

/** A heading inside a sheet ("MIC", "VIDEO"). */
@Composable
fun SectionTitle(text: String) {
    Text(text.uppercase(), color = Palette.muted, fontSize = 11.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 0.6.sp)
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
        Modifier.fillMaxWidth().clip(RoundedCornerShape(8.dp)).background(Palette.base)
            .border(1.dp, if (on) Palette.blueLight else Palette.base, RoundedCornerShape(8.dp))
            .clickable(enabled = enabled, onClick = onClick).padding(horizontal = 12.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text(text, color = if (enabled) Palette.text else Palette.muted, fontSize = 14.sp, modifier = Modifier.weight(1f))
        trailing()
    }
}
