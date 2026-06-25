package com.aimassistant

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.util.DisplayMetrics
import android.view.WindowManager
import androidx.core.app.NotificationCompat
import com.aimassistant.capture.FrameCapture
import com.aimassistant.capture.ScreenCapture
import com.aimassistant.capture.SurfaceControlCapture
import com.aimassistant.overlay.OverlayManager
import com.aimassistant.trigger.FireDetector
import com.aimassistant.util.RootUtil
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import java.nio.ByteBuffer

/**
 * 单一前台 Service: 后台运行，串联 屏幕捕获 -> NCNN 推理 -> 悬浮窗 -> 触摸注入。
 *
 * 启动方式:
 *   1. MainActivity 申请 悬浮窗 + MediaProjection 权限
 *   2. 将 resultCode + Intent 通过 startForegroundService 传入
 *
 * 生命周期内持续推理，悬浮窗实时显示检测框与 FPS。
 */
class AimService : Service() {

    companion object {
        const val EXTRA_RESULT_CODE = "result_code"
        const val EXTRA_RESULT_DATA = "result_data"
        const val EXTRA_USE_GPU = "use_gpu"
        const val EXTRA_TRIGGER = "trigger"
        /** 捕获后端: "surfacecontrol" 或 "mediaprojection" */
        const val EXTRA_CAPTURE_MODE = "capture_mode"
        /** 是否启用自动开火检测 */
        const val EXTRA_AUTO_FIRE = "auto_fire"
        /** 注入后端: 0=uinput, 1=内核驱动, 2=陀螺仪, -1=自动探测 */
        const val EXTRA_INJECT_BACKEND = "inject_backend"
        private const val CHANNEL_ID = "aim_service"
        private const val NOTIF_ID = 1
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private var loopJob: Job? = null
    private var wakeLock: PowerManager.WakeLock? = null

    private var capture: FrameCapture? = null
    private lateinit var overlay: OverlayManager
    private var fireDetector: FireDetector? = null
    private var initialized = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForegroundCompat()

        if (intent != null && !initialized) {
            val resultCode = intent.getIntExtra(EXTRA_RESULT_CODE, 0)
            val data: Intent? = intent.getParcelableExtra(EXTRA_RESULT_DATA)
            val useGpu = intent.getBooleanExtra(EXTRA_USE_GPU, false)
            val trigger = intent.getBooleanExtra(EXTRA_TRIGGER, false)
            val captureMode = intent.getStringExtra(EXTRA_CAPTURE_MODE) ?: "mediaprojection"
            val autoFire = intent.getBooleanExtra(EXTRA_AUTO_FIRE, false)
            val injectBackend = intent.getIntExtra(EXTRA_INJECT_BACKEND, -1)
            startPipeline(resultCode, data, useGpu, trigger, captureMode, autoFire, injectBackend)
        }
        return START_STICKY
    }

    private fun startPipeline(
        resultCode: Int, data: Intent?, useGpu: Boolean, trigger: Boolean,
        captureMode: String, autoFire: Boolean, injectBackend: Int
    ) {
        // 1. 屏幕分辨率
        val wm = getSystemService(Context.WINDOW_SERVICE) as WindowManager
        val dm = DisplayMetrics()
        @Suppress("DEPRECATION")
        wm.defaultDisplay.getRealMetrics(dm)
        val screenW = dm.widthPixels
        val screenH = dm.heightPixels

        // 2. 拷贝模型到内部存储
        val paramFile = RootUtil.copyAssetToFile(this, "models/yolov8n.param")
        val binFile = RootUtil.copyAssetToFile(this, "models/yolov8n.bin")

        // 3. 初始化原生层（加载模型 + 创建注入设备）
        //    注入后端自动探测: 内核驱动 -> 陀螺仪 -> uinput
        //    injectBackend: 0=uinput, 1=内核驱动, 2=陀螺仪, -1=自动
        val ok = NativeBridge.nativeInit(
            paramFile.absolutePath, binFile.absolutePath,
            useGpu, 4, screenW, screenH,
            preferredBackend = injectBackend
        )
        if (!ok) {
            android.util.Log.e("AimService", "nativeInit 失败，请检查模型文件与 Root 权限")
        } else {
            val backendName = NativeBridge.nativeGetBackendName()
            android.util.Log.i("AimService", "注入后端: $backendName")
        }
        NativeBridge.nativeSetConfig(
            aimRadius = 400f, aimSpeed = 0.35f, leadFactor = 1f,
            pipelineMs = 50f, headshot = false, trigger = trigger
        )

        // 4. 悬浮窗
        overlay = OverlayManager(this).also { it.show() }

        // 5. 屏幕捕获: 优先 SurfaceControl，失败回退 MediaProjection
        capture = when (captureMode) {
            "surfacecontrol" -> {
                val sc = SurfaceControlCapture(this)
                if (sc.isAvailable()) {
                    android.util.Log.i("AimService", "使用 SurfaceControl 捕获")
                    sc
                } else {
                    android.util.Log.w("AimService", "SurfaceControl 不可用，回退到 MediaProjection")
                    createMediaProjectionCapture(resultCode, data)
                }
            }
            else -> createMediaProjectionCapture(resultCode, data)
        }
        capture?.onFrame = { buffer, w, h -> onFrame(buffer, w, h) }
        val started = capture?.start() ?: false
        if (!started) {
            android.util.Log.e("AimService", "屏幕捕获启动失败")
        }

        // 6. 开火检测器（自动触发瞄准）
        if (autoFire) {
            fireDetector = FireDetector(screenW, screenH).also { fd ->
                fd.onFire = { onFireDetected() }
                fd.start(autoAimEnabled = true)
            }
            android.util.Log.i("AimService", "开火检测已启用")
        }

        // 7. 唤醒锁，保持推理
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "AimAssistant::Loop")
        wakeLock?.acquire(10 * 60 * 1000L)  // 10 分钟

        initialized = true
    }

    /** 创建 MediaProjection 捕获实例（需授权结果）。 */
    private fun createMediaProjectionCapture(resultCode: Int, data: Intent?): FrameCapture? {
        if (data == null) return null
        val sc = ScreenCapture(this)
        sc.setPermissionResult(resultCode, data)
        return sc
    }

    /** 开火检测回调: 触发一次瞄准注入。 */
    private fun onFireDetected() {
        // 使用屏幕中心作为瞄准起点
        val w = capture?.width ?: return
        val h = capture?.height ?: return
        NativeBridge.nativeAimOnce(w / 2f, h / 2f)
    }

    private var lastFpsTime = 0L
    private var frameCount = 0
    private var currentFps = 0f

    private fun onFrame(buffer: ByteBuffer, width: Int, height: Int) {
        // 喂给开火检测器（亮度尖峰检测，非阻塞）
        fireDetector?.feedFrame(buffer, width, height)

        if (loopJob?.isActive == true) return  // 串行化，避免积压
        loopJob = scope.launch {
            // 推理
            val dets = NativeBridge.nativeDetect(buffer, width, height, 0.45f, 0.45f)
            val infMs = NativeBridge.nativeGetInferenceMs()

            // 解析检测框
            val boxes = ArrayList<OverlayManager.Box>()
            var bestIdx = -1
            var bestDist = Float.MAX_VALUE
            val cx = width / 2f
            val cy = height / 2f
            val n = dets.size / 6
            for (i in 0 until n) {
                val base = i * 6
                val x1 = dets[base]; val y1 = dets[base + 1]
                val x2 = dets[base + 2]; val y2 = dets[base + 3]
                val conf = dets[base + 4]; val cls = dets[base + 5].toInt()
                val bcx = (x1 + x2) / 2f
                val bcy = (y1 + y2) / 2f
                val d = (bcx - cx) * (bcx - cx) + (bcy - cy) * (bcy - cy)
                if (d < bestDist) { bestDist = d; bestIdx = i }
                boxes.add(OverlayManager.Box(x1, y1, x2, y2, conf, cls, false))
            }
            if (bestIdx in boxes.indices) {
                val b = boxes[bestIdx]
                boxes[bestIdx] = b.copy(selected = true)
                // 计算目标相对准星的偏移
                val targetCx = (b.x1 + b.x2) / 2f
                val targetCy = (b.y1 + b.y2) / 2f
                val dx = targetCx - cx
                val dy = targetCy - cy

                // 根据后端选择注入方式:
                //   - 陀螺仪后端: 每帧持续微调（无触摸事件）
                //   - 触摸后端: 仅在无开火检测器时自动注入
                val backendName = NativeBridge.nativeGetBackendName()
                if (backendName.contains("陀螺仪")) {
                    // 陀螺仪: 每帧微调，无需开火触发
                    NativeBridge.nativeGyroAim(dx, dy)
                } else if (fireDetector == null) {
                    // 触摸注入: 开火检测器启用时由 onFireDetected() 触发
                    NativeBridge.nativeAimOnce(cx, cy)
                }
            }

            // FPS 统计
            frameCount++
            val now = System.currentTimeMillis()
            if (now - lastFpsTime >= 1000) {
                currentFps = frameCount * 1000f / (now - lastFpsTime)
                frameCount = 0
                lastFpsTime = now
            }

            // 更新悬浮窗（切到主线程）
            overlay.updateDetections(boxes, currentFps, infMs)
        }
    }

    private fun startForegroundCompat() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID, "Aim Service", NotificationManager.IMPORTANCE_LOW
            )
            (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager)
                .createNotificationChannel(channel)
        }
        val notif: Notification = NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("AimAssistant 运行中")
            .setContentText("屏幕捕获与推理已启动")
            .setSmallIcon(android.R.drawable.ic_menu_view)
            .setOngoing(true)
            .build()
        startForeground(NOTIF_ID, notif)
    }

    override fun onDestroy() {
        super.onDestroy()
        loopJob?.cancel()
        scope.cancel()
        if (initialized) {
            fireDetector?.stop()
            fireDetector = null
            capture?.stop()
            capture = null
            overlay.hide()
            NativeBridge.nativeShutdown()
        }
        wakeLock?.release()
    }
}
