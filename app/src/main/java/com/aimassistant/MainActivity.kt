package com.aimassistant

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.widget.Button
import android.widget.CheckBox
import android.widget.RadioGroup
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import com.aimassistant.capture.ScreenCapture
import com.aimassistant.capture.SurfaceControlCapture
import com.aimassistant.util.RootUtil

/**
 * 入口 Activity: 申请权限并启动 AimService。
 *
 * 权限链:
 *   1. 悬浮窗权限 (SYSTEM_ALERT_WINDOW)
 *   2. Root 检测（用于 /dev/uinput）
 *   3. MediaProjection 屏幕捕获授权（仅 MediaProjection 模式需要）
 *
 * 可选项:
 *   - 捕获后端: SurfaceControl（低延迟，无需授权）/ MediaProjection
 *   - 自动开火: 检测开火时自动触发瞄准
 */
class MainActivity : AppCompatActivity() {

    private lateinit var statusText: TextView
    private lateinit var startBtn: Button
    private lateinit var captureModeGroup: RadioGroup
    private lateinit var injectBackendGroup: RadioGroup
    private lateinit var autoFireCheck: CheckBox
    private lateinit var useGpuCheck: CheckBox
    private lateinit var capture: ScreenCapture
    private var useGpu = false
    private var trigger = true

    // MediaProjection 授权回调
    private val projectionLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == RESULT_OK && result.data != null) {
            startAimService(result.resultCode, result.data!!)
        } else {
            statusText.text = "屏幕捕获授权被拒绝"
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        statusText = findViewById(R.id.statusText)
        startBtn = findViewById(R.id.startBtn)
        captureModeGroup = findViewById(R.id.captureModeGroup)
        injectBackendGroup = findViewById(R.id.injectBackendGroup)
        autoFireCheck = findViewById(R.id.autoFireCheck)
        useGpuCheck = findViewById(R.id.useGpuCheck)
        capture = ScreenCapture(this)

        startBtn.setOnClickListener {
            startPipeline()
        }

        updateStatus()
    }

    private fun startPipeline() {
        // 1. 悬浮窗权限
        if (!Settings.canDrawOverlays(this)) {
            statusText.text = "请授予悬浮窗权限后重试"
            val intent = Intent(
                Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                Uri.parse("package:$packageName")
            )
            startActivity(intent)
            return
        }

        // 2. Root 检测
        if (!RootUtil.isRooted()) {
            statusText.text = "未检测到 Root，触摸注入将不可用"
        }

        // 3. 通知权限（Android 13+）
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            requestPermissions(arrayOf(android.Manifest.permission.POST_NOTIFICATIONS), 1)
        }

        useGpu = useGpuCheck.isChecked
        val autoFire = autoFireCheck.isChecked
        val useSurfaceControl = captureModeGroup.checkedRadioButtonId == R.id.rbSurfaceControl

        // 4. SurfaceControl 模式无需 MediaProjection 授权，直接启动
        if (useSurfaceControl && SurfaceControlCapture(this).isAvailable()) {
            startAimService(0, Intent(), useSurfaceControl = true, autoFire = autoFire)
            return
        }

        // 5. MediaProjection 授权
        projectionLauncher.launch(capture.createPermissionIntent())
    }

    private fun startAimService(
        resultCode: Int, data: Intent,
        useSurfaceControl: Boolean = false, autoFire: Boolean = false
    ) {
        val captureMode = if (useSurfaceControl) "surfacecontrol" else "mediaprojection"
        // 注入后端: 0=uinput, 1=内核驱动, 2=陀螺仪, 3=TwT驱动（手动选择，默认 TwT）
        val injectBackend = when (injectBackendGroup.checkedRadioButtonId) {
            R.id.rbInjectTwt -> 3
            R.id.rbInjectKernel -> 1
            R.id.rbInjectGyro -> 2
            R.id.rbInjectUinput -> 0
            else -> 3  // 默认 TwT
        }
        val intent = Intent(this, AimService::class.java).apply {
            putExtra(AimService.EXTRA_RESULT_CODE, resultCode)
            putExtra(AimService.EXTRA_RESULT_DATA, data)
            putExtra(AimService.EXTRA_USE_GPU, useGpu)
            putExtra(AimService.EXTRA_TRIGGER, trigger)
            putExtra(AimService.EXTRA_CAPTURE_MODE, captureMode)
            putExtra(AimService.EXTRA_AUTO_FIRE, autoFire)
            putExtra(AimService.EXTRA_INJECT_BACKEND, injectBackend)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(intent)
        } else {
            startService(intent)
        }
        statusText.text = "服务已启动（捕获=$captureMode, 注入后端=$injectBackend, GPU=$useGpu）"
        Toast.makeText(this, "AimService 已启动", Toast.LENGTH_SHORT).show()
    }

    private fun updateStatus() {
        val rooted = RootUtil.isRooted()
        val overlay = Settings.canDrawOverlays(this)
        val scAvailable = SurfaceControlCapture(this).isAvailable()
        statusText.text = buildString {
            append("Root: $rooted\n")
            append("悬浮窗: $overlay\n")
            append("SurfaceControl: $scAvailable\n")
            append("分辨率: ${capture.width}x${capture.height}")
        }
    }

    override fun onResume() {
        super.onResume()
        updateStatus()
    }
}
