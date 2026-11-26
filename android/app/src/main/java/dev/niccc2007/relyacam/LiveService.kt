package dev.niccc2007.relyacam

import android.app.Application
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat

class RelyApp : Application() {
    override fun onCreate() {
        super.onCreate()
        Engine.init(this)
    }
}

/**
 * Keeps the process in the foreground while streaming, with a notification that can stop it. It
 * owns nothing itself: the engine holds the camera, and stopping the stream stops this service,
 * which removes the notification (the must-never list: nothing runs after Stop).
 */
class LiveService : Service() {
    private var wake: PowerManager.WakeLock? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            Engine.stop()
            return START_NOT_STICKY
        }
        val pc = intent?.getStringExtra(EXTRA_PC).orEmpty()
        val mic = intent?.getBooleanExtra(EXTRA_MIC, false) == true
        channel()
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE,
        )
        val stop = PendingIntent.getService(
            this, 1, Intent(this, LiveService::class.java).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE,
        )
        val n: Notification = NotificationCompat.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.presence_video_online)
            .setContentTitle("Live to OBS")
            .setContentText(if (pc.isEmpty()) "Rely-A-Cam is streaming" else "Streaming to $pc")
            .setOngoing(true)
            .setContentIntent(open)
            .addAction(0, "Stop", stop)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .build()
        val type = if (Build.VERSION.SDK_INT >= 30) {
            ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA or (if (mic) ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE else 0)
        } else 0
        ServiceCompat.startForeground(this, 7, n, type)
        if (wake == null) {
            wake = getSystemService(PowerManager::class.java)
                .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "relyacam:live").apply { acquire(6 * 60 * 60 * 1000L) }
        }
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        wake?.let { if (it.isHeld) it.release() }
        wake = null
        super.onDestroy()
    }

    private fun channel() {
        val nm = getSystemService(NotificationManager::class.java)
        if (nm.getNotificationChannel(CHANNEL) == null) {
            nm.createNotificationChannel(NotificationChannel(CHANNEL, "Streaming", NotificationManager.IMPORTANCE_LOW))
        }
    }

    companion object {
        private const val CHANNEL = "live"
        private const val ACTION_STOP = "dev.niccc2007.relyacam.STOP"
        private const val EXTRA_PC = "pc"
        private const val EXTRA_MIC = "mic"

        fun start(context: Context, pc: String, mic: Boolean) {
            val i = Intent(context, LiveService::class.java).putExtra(EXTRA_PC, pc).putExtra(EXTRA_MIC, mic)
            runCatching { ContextCompat.startForegroundService(context, i) }
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, LiveService::class.java))
        }
    }
}
