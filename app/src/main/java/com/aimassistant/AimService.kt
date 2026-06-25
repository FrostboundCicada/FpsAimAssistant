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
import android.util.Log
import android.view.WindowManager
import androidx.core.app.NotificationCompat
import com.aimassistant.capture.FrameCapture
import com.aimassistant.capture.ScreenCapture
import com.aimassistant.capture.SurfaceControlCapture
import com.aimassistant.overlay.OverlayManager
import com.aimassistant.trigger.FireDetector
import com.aimassistant.util.DriverProbe
import com.aimassistant.util.ModelManager
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
 * 启动时执行驱动检测，结果通过悬浮窗控制面板展示。
 * 支持运行时通过悬浮窗切换模型 / 开关测试模式。
 */
class AimService : Service() {

    companion object {
        private const val TAG = "AimService"
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
        /** 初始模型 key（ModelManager.ModelInfo.key），未指定时用列表第一个 */
        const val EXTRA_INITIAL_MODEL_KEY = "initial_model_key"
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

    // 模型管理
    private var availableModels: List<ModelManager.ModelInfo> = emptyList()
    private var currentModelIndex: Int = 0
    @Volatile private var useGpu: Boolean = false

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
            val initialModelKey = intent.getStringExtra(EXTRA_INITIAL_MODEL_KEY)
            startPipeline(resultCode, data, useGpu, trigger, captureMode, autoFire, injectBackend, initialModelKey)
        }
        return START_STICKY
    }

    private fun startPipeline(
        resultCode: Int, data: Intent?, useGpu: Boolean, trigger: Boolean,
        captureMode: String, autoFire: Boolean, injectBackend: Int,
        initialModelKey: String?
    ) {
        this.useGpu = useGpu

        // 1. 屏幕分辨率
        val wm = getSystemService(Context.WINDOW_SERVICE) as WindowManager
        val dm = DisplayMetrics()
        @Suppress("DEPRECATION")
        wm.defaultDisplay.getRealMetrics(dm)
        val screenW = dm.widthPixels
        val screenH = dm.heightPixels

        // 2. 扫描可用模型
        availableModels = ModelManager.scanModels(this)
        if (availableModels.isEmpty()) {
            Log.e(TAG, "未找到任何模型文件，请将 .param/.bin 放入 assets/models/")
        }
        // 选择初始模型
        currentModelIndex = if (initialModelKey != null) {
            availableModels.indexOfFirst { it.key == initialModelKey }.let { if (it < 0) 0 else it }
        } else 0
        val initialModel = availableModels.getOrNull(currentModelIndex)

        // 3. 驱动检测（在推理启动前执行）
        val driverReport = try {
            DriverProbe.probe()
        } catch (e: Exception) {
            Log.w(TAG, "驱动检测异常: ${e.message}")
            null
        }
        Log.i(TAG, "驱动检测: ${driverReport?.let { if (it.anyAvailable) "可用" else "不可用" } ?: "失败"}")

        // 4. 悬浮窗（先创建，便于显示后续状态）
        overlay = OverlayManager(this).also { ov ->
            ov.show()
            ov.listener = object : OverlayManager.Listener {
                override fun onTestModeToggled(enabled: Boolean) {
                    Log.i(TAG, "测试模式: ${if (enabled) "开" else "关"}")
                    // 测试模式仅影响 UI 显示，无需通知 native
                }
                override fun onCycleModel(): String? {
                    return switchToNextModel()
                }
                override fun onExit() {
                    Log.i(TAG, "用户请求退出")
                    stopSelf()
                }
            }
            // 显示驱动检测结果
            driverReport?.let { ov.setDriverReport(it) }
        }

        // 5. 初始化原生层（加载模型 + 创建注入设备）
        if (initialModel != null) {
            val paramFile = RootUtil.copyAssetToFile(this, initialModel.paramAsset)
            val binFile = RootUtil.copyAssetToFile(this, initialModel.binAsset)
            val ok = NativeBridge.nativeInit(
                paramPath = paramFile.absolutePath,
                binPath = binFile.absolutePath,
                useGpu = useGpu,
                numThreads = 4,
                screenW = screenW,
                screenH = screenH,
                preferredBackend = injectBackend,
                inputBlob = initialModel.inputBlob,
                outputBlob = initialModel.outputBlob,
                inputSize = initialModel.inputSize,
                numClasses = initialModel.numClasses,
                formatInt = initialModel.formatInt,
                bboxXywh = initialModel.bboxXywh
            )
            if (!ok) {
                Log.e(TAG, "nativeInit 失败，请检查模型文件与 Root 权限")
            } else {
                overlay.setModelName(initialModel.displayName)
                val backendName = NativeBridge.nativeGetBackendName()
                overlay.setBackendName(backendName)
                Log.i(TAG, "初始模型: ${initialModel.displayName}, 注入后端: $backendName")
            }
        } else {
            Log.e(TAG, "无可用模型，跳过 nativeInit")
        }
        NativeBridge.nativeSetConfig(
            aimRadius = 400f, aimSpeed = 0.35f, leadFactor = 1f,
            pipelineMs = 50f, headshot = false, trigger = trigger
        )

        // 6. 屏幕捕获: 优先 SurfaceControl，失败回退 MediaProjection
        capture = when (captureMode) {
            "surfacecontrol" -> {
                val sc = SurfaceControlCapture(this)
                if (sc.isAvailable()) {
                    Log.i(TAG, "使用 SurfaceControl 捕获")
                    sc
                } else {
                    Log.w(TAG, "SurfaceControl 不可用，回退到 MediaProjection")
                    createMediaProjectionCapture(resultCode, data)
                }
            }
            else -> createMediaProjectionCapture(resultCode, data)
        }
        capture?.onFrame = { buffer, w, h -> onFrame(buffer, w, h) }
        val started = capture?.start() ?: false
        if (!started) {
            Log.e(TAG, "屏幕捕获启动失败")
        }

        // 7. 开火检测器（自动触发瞄准）
        if (autoFire) {
            fireDetector = FireDetector(screenW, screenH).also { fd ->
                fd.onFire = { onFireDetected() }
                fd.start(autoAimEnabled = true)
            }
            Log.i(TAG, "开火检测已启用")
        }

        // 8. 唤醒锁，保持推理
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "AimAssistant::Loop")
        wakeLock?.acquire(10 * 60 * 1000L)  // 10 分钟

        initialized = true
    }

    /** 切换到下一个可用模型。返回新模型显示名，失败返回 null。 */
    private fun switchToNextModel(): String? {
        if (availableModels.isEmpty()) return null
        val nextIndex = (currentModelIndex + 1) % availableModels.size
        val nextModel = availableModels[nextIndex]
        val paramFile = try {
            RootUtil.copyAssetToFile(this, nextModel.paramAsset)
        } catch (e: Exception) {
            Log.e(TAG, "拷贝模型 param 失败: ${e.message}")
            return null
        }
        val binFile = try {
            RootUtil.copyAssetToFile(this, nextModel.binAsset)
        } catch (e: Exception) {
            Log.e(TAG, "拷贝模型 bin 失败: ${e.message}")
            return null
        }
        val ok = NativeBridge.nativeLoadModel(
            paramPath = paramFile.absolutePath,
            binPath = binFile.absolutePath,
            inputBlob = nextModel.inputBlob,
            outputBlob = nextModel.outputBlob,
            inputSize = nextModel.inputSize,
            numClasses = nextModel.numClasses,
            formatInt = nextModel.formatInt,
            bboxXywh = nextModel.bboxXywh,
            displayName = nextModel.displayName
        )
        if (!ok) {
            Log.e(TAG, "切换模型失败: ${nextModel.displayName}")
            return null
        }
        currentModelIndex = nextIndex
        overlay.setModelName(nextModel.displayName)
        Log.i(TAG, "已切换模型: ${nextModel.displayName}")
        return nextModel.displayName
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
        val w = capture?.width ?: return
        val h = capture?.height ?: return
        NativeBridge.nativeAimOnce(w / 2f, h / 2f)
    }

    private var lastFpsTime = 0L
    private var frameCount = 0
    private var currentFps = 0f

    private fun onFrame(buffer: ByteBuffer, width: Int, height: Int) {
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
                val targetCx = (b.x1 + b.x2) / 2f
                val targetCy = (b.y1 + b.y2) / 2f
                val dx = targetCx - cx
                val dy = targetCy - cy

                val backendName = NativeBridge.nativeGetBackendName()
                if (backendName.contains("陀螺仪")) {
                    NativeBridge.nativeGyroAim(dx, dy)
                } else if (fireDetector == null) {
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

            // 更新悬浮窗
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
