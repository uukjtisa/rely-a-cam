package dev.niccc2007.relyacam.ui

import android.graphics.Matrix
import android.graphics.SurfaceTexture
import android.view.TextureView
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.viewinterop.AndroidView
import dev.niccc2007.relyacam.Engine
import dev.niccc2007.relyacam.UiState
import dev.niccc2007.relyacam.camera.CameraInfo
import dev.niccc2007.relyacam.camera.Upright
import kotlinx.coroutines.delay

/**
 * The camera picture, full-bleed, with pinch-to-zoom and tap-to-focus. Shows exactly what OBS
 * shows (mirror and flip included), turned for the screen.
 */
@Composable
fun CameraView(s: UiState, applyMirror: Boolean, modifier: Modifier = Modifier) {
    var view by remember { mutableStateOf<TextureView?>(null) }
    var ring by remember { mutableStateOf<Offset?>(null) }
    var ringKey by remember { mutableStateOf(0) }
    // The newest state, for the layout listener: a turn of the phone resizes the view without
    // changing any state, and the transform must follow it.
    val latest = remember { arrayOfNulls<Any>(2) }
    latest[0] = s
    latest[1] = applyMirror

    Box(modifier.fillMaxSize()) {
        AndroidView(
            modifier = Modifier.fillMaxSize(),
            factory = { ctx ->
                TextureView(ctx).apply {
                    surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                        override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) = Engine.attachPreview(st)
                        override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) {}
                        override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean { Engine.detachPreview(); return true }
                        override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}
                    }
                    addOnLayoutChangeListener { v, _, _, _, _, _, _, _, _ ->
                        val st = latest[0] as UiState
                        val cam = st.cam ?: return@addOnLayoutChangeListener
                        val tv = v as TextureView
                        if (tv.width > 0) tv.setTransform(matrix(tv, st, cam, latest[1] as Boolean))
                    }
                    view = this
                }
            },
            update = { tv ->
                val cam = s.cam ?: return@AndroidView
                tv.post {
                    if (tv.width > 0) tv.setTransform(matrix(tv, s, cam, applyMirror))
                }
            },
        )
        Box(
            Modifier.fillMaxSize()
                .pointerInput(Unit) {
                    detectTransformGestures { _, _, zoom, _ -> if (zoom != 1f) Engine.pinch(zoom) }
                }
                .pointerInput(s.cam?.id, s.mirror, s.flip, s.previewW, applyMirror) {
                    detectTapGestures { p ->
                        val tv = view ?: return@detectTapGestures
                        val cam = s.cam ?: return@detectTapGestures
                        val inv = Matrix()
                        if (!matrix(tv, s, cam, applyMirror).invert(inv)) return@detectTapGestures
                        val pts = floatArrayOf(p.x, p.y)
                        inv.mapPoints(pts)
                        var cx = (pts[0] / tv.width).coerceIn(0f, 1f)
                        val cy = (pts[1] / tv.height).coerceIn(0f, 1f)
                        // The preview of a front camera arrives mirrored; the sensor's frame is not.
                        if (cam.front) cx = 1 - cx
                        val (sx, sy) = Upright.toSensor(cx, cy, cam.sensorOrientation, mirror = false, flip = false)
                        Engine.focusSensor(sx, sy)
                        ring = p
                        ringKey++
                    }
                },
        )
        ring?.let { FocusRing(it, ringKey) { ring = null } }
    }
}

private fun matrix(tv: TextureView, s: UiState, cam: CameraInfo, applyMirror: Boolean): Matrix {
    val rotation = (tv.display?.rotation ?: 0) * 90
    return Upright.previewMatrix(
        tv.width.toFloat(), tv.height.toFloat(), s.previewW, s.previewH, cam, rotation,
        mirror = applyMirror && s.mirror, flip = applyMirror && s.flip,
    )
}

@Composable
private fun FocusRing(at: Offset, key: Int, gone: () -> Unit) {
    var shown by remember(key) { mutableStateOf(true) }
    val scale by animateFloatAsState(if (shown) 1f else 0.8f, label = "ring")
    LaunchedEffect(key) { delay(900); shown = false; delay(200); gone() }
    Canvas(Modifier.fillMaxSize()) {
        drawCircle(Color.White.copy(alpha = if (shown) 0.9f else 0f), radius = 34.dp(this) * scale, center = at, style = Stroke(width = 2.dp(this)))
    }
}

private fun Int.dp(d: androidx.compose.ui.unit.Density) = with(d) { this@dp.toFloat() * density }
