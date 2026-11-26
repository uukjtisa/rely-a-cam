package dev.niccc2007.relyacam

import android.Manifest
import android.content.Intent
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.lifecycleScope
import dev.niccc2007.relyacam.ui.App
import dev.niccc2007.relyacam.update.Updater
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainActivity : ComponentActivity() {

    private val permissions = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { report() }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        Engine.init(this)
        handle(intent)
        setContent {
            val s by Engine.ui.collectAsStateWithLifecycle()
            LaunchedEffect(s.screen, s.black, s.lock) { window(s) }
            App(s) { ask() }
        }
        if (granted(Manifest.permission.CAMERA)) report() else ask()
        checkForUpdate()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handle(intent)
    }

    override fun onStart() { super.onStart(); Engine.onVisible(true) }
    override fun onStop() { Engine.onVisible(false); super.onStop() }

    private fun handle(i: Intent?) {
        val data = i?.data?.toString() ?: return
        if (data.startsWith("relyacam://", ignoreCase = true)) Engine.onLink(data)
    }

    private fun granted(p: String) = ContextCompat.checkSelfPermission(this, p) == PackageManager.PERMISSION_GRANTED

    private fun ask() {
        val list = mutableListOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO)
        if (Build.VERSION.SDK_INT >= 33) list += Manifest.permission.POST_NOTIFICATIONS
        permissions.launch(list.toTypedArray())
    }

    private fun report() = Engine.onPermissions(granted(Manifest.permission.CAMERA), granted(Manifest.permission.RECORD_AUDIO))

    /** Screen on while live, dim in black-screen mode, and turning locked when the stream is. */
    private fun window(s: UiState) {
        val live = s.screen == Screen.LIVE
        if (live) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        window.attributes = window.attributes.apply {
            screenBrightness = if (live && s.black) 0.01f else WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
        }
        requestedOrientation = if (live && s.lock) ActivityInfo.SCREEN_ORIENTATION_LOCKED
        else ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR
    }

    /** At most twice a day, quietly; offline just means no banner. */
    private fun checkForUpdate() {
        val prefs = Engine.prefs
        if (System.currentTimeMillis() - prefs.lastUpdateCheck < 12 * 60 * 60 * 1000L) return
        lifecycleScope.launch {
            val r = withContext(Dispatchers.IO) { Updater.check() }
            prefs.lastUpdateCheck = System.currentTimeMillis()
            Engine.setUpdate(r)
        }
    }
}
