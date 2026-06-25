package com.aimassistant

import android.content.Intent
import android.graphics.Color
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.view.View
import android.widget.ArrayAdapter
import android.widget.AutoCompleteTextView
import android.widget.CheckBox
import android.widget.RadioGroup
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import com.aimassistant.capture.ScreenCapture
import com.aimassistant.capture.SurfaceControlCapture
import com.aimassistant.util.DriverProbe
import com.aimassistant.util.ModelManager
import com.aimassistant.util.RootUtil
import com.google.android.material.button.MaterialButton

/**
 * 入口 Activity: 申请权限、检测驱动/模型、启动 AimService。
 *
 * UI 结构（Material3 深色卡片）:
 *   1. 运行环境状态卡片（Root/悬浮窗/SurfaceControl/分辨率）
 *   2. 驱动检测卡片（内核/TwT/陀螺仪/uinput，可重新检测）
 *   3. 模型选择卡片（下拉，运行时可在悬浮窗切换）
 *   4. 屏幕捕获后端
 *   5. 触摸注入后端 + GPU/自动开火
 *   6. 启动按钮
 *
 * 启动时自动执行驱动检测，结果通过卡片展示，并随 Intent 传入 AimService。
 */
class MainActivity : AppCompatActivity() {

    private lateinit var textRootState: TextView
    private lateinit var textOverlayState: TextView
    private lateinit var textScState: TextView
    private lateinit var textResolution: TextView

    private lateinit var textDriverKernel: TextView
    private lateinit var textDriverTwt: TextView
    private lateinit var textDriverGyro: TextView
    private lateinit var textDriverUinput: TextView
    private lateinit var textDriverDetail: TextView
    private lateinit var btnReprobe: MaterialButton

    private lateinit var modelDropdown: AutoCompleteTextView
    private lateinit var textModelInfo: TextView

    private lateinit var startBtn: MaterialButton
    private lateinit var captureModeGroup: RadioGroup
    private lateinit var injectBackendGroup: RadioGroup
    private lateinit var autoFireCheck: CheckBox
    private lateinit var useGpuCheck: CheckBox

    private lateinit var capture: ScreenCapture
    private var useGpu = false
    private var trigger = true

    private var availableModels: List<ModelManager.ModelInfo> = emptyList()
    private var selectedModelIndex = 0

    // MediaProjection 授权回调
    private val projectionLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == RESULT_OK && result.data != null) {
            startAimService(result.resultCode, result.data!!)
        } else {
            Toast.makeText(this, "屏幕捕获授权被拒绝", Toast.LENGTH_SHORT).show()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        bindViews()
        capture = ScreenCapture(this)

        startBtn.setOnClickListener { startPipeline() }
        btnReprobe.setOnClickListener { runDriverProbe() }

        // 首次进入: 扫描模型 + 驱动检测 + 环境状态
        scanModels()
        runDriverProbe()
        updateEnvStatus()
    }

    private fun bindViews() {
        textRootState = findViewById(R.id.textRootState)
        textOverlayState = findViewById(R.id.textOverlayState)
        textScState = findViewById(R.id.textScState)
        textResolution = findViewById(R.id.textResolution)

        textDriverKernel = findViewById(R.id.textDriverKernel)
        textDriverTwt = findViewById(R.id.textDriverTwt)
        textDriverGyro = findViewById(R.id.textDriverGyro)
        textDriverUinput = findViewById(R.id.textDriverUinput)
        textDriverDetail = findViewById(R.id.textDriverDetail)
        btnReprobe = findViewById(R.id.btnReprobe)

        modelDropdown = findViewById(R.id.modelDropdown)
        textModelInfo = findViewById(R.id.textModelInfo)

        startBtn = findViewById(R.id.startBtn)
        captureModeGroup = findViewById(R.id.captureModeGroup)
        injectBackendGroup = findViewById(R.id.injectBackendGroup)
        autoFireCheck = findViewById(R.id.autoFireCheck)
        useGpuCheck = findViewById(R.id.useGpuCheck)
    }

    /** 扫描 assets/models/ 并填充下拉框。 */
    private fun scanModels() {
        availableModels = ModelManager.scanModels(this)
        if (availableModels.isEmpty()) {
            modelDropdown.setText(getString(R.string.model_empty), false)
            textModelInfo.text = "—"
            textModelInfo.visibility = View.GONE
            return
        }
        val displayNames = availableModels.map { it.displayName }
        val adapter = ArrayAdapter(this, android.R.layout.simple_dropdown_item_1line, displayNames)
        modelDropdown.setAdapter(adapter)
        modelDropdown.setText(displayNames.getOrElse(0) { "" }, false)
        selectedModelIndex = 0
        updateModelInfoText(availableModels[0])

        modelDropdown.setOnItemClickListener { _, _, position, _ ->
            if (position in availableModels.indices) {
                selectedModelIndex = position
                updateModelInfoText(availableModels[position])
            }
        }
    }

    private fun updateModelInfoText(m: ModelManager.ModelInfo) {
        val formatStr = when (m.formatInt) {
            NativeBridge.FORMAT_DECODED -> "decoded"
            else -> "dfl_raw"
        }
        textModelInfo.text = "尺寸=%d 类别=%d 格式=%s\nin=%s out=%s bbox_xywh=%b".format(
            m.inputSize, m.numClasses, formatStr,
            m.inputBlob, m.outputBlob, m.bboxXywh
        )
        textModelInfo.visibility = View.VISIBLE
    }

    /** 执行驱动检测并刷新 UI。 */
    private fun runDriverProbe() {
        val report = try {
            DriverProbe.probe()
        } catch (e: Exception) {
            null
        }
        if (report == null) {
            setBackendState(textDriverKernel, false, "异常")
            setBackendState(textDriverTwt, false, "异常")
            setBackendState(textDriverGyro, false, "异常")
            setBackendState(textDriverUinput, false, "异常")
            textDriverDetail.text = "驱动检测异常"
            return
        }
        setBackendState(textDriverKernel, report.kernelDriver.available, report.kernelDriver.detail)
        setBackendState(textDriverTwt, report.twtDriver.available, report.twtDriver.detail)
        setBackendState(textDriverGyro, report.gyroscope.available, report.gyroscope.detail)
        setBackendState(textDriverUinput, report.uinput.available, report.uinput.detail)

        val sb = StringBuilder()
        if (report.loadedModules.isNotEmpty()) {
            sb.append("已加载模块: ").append(report.loadedModules.joinToString(", "))
        } else {
            sb.append("/proc/modules 未命中已知模块")
        }
        sb.append("\n综合: ")
        sb.append(if (report.anyAvailable) "存在可用注入后端" else getString(R.string.driver_none))
        textDriverDetail.text = sb.toString()
    }

    private fun setBackendState(view: TextView, available: Boolean, detail: String) {
        if (available) {
            view.text = getString(R.string.state_ok)
            view.setTextColor(Color.parseColor("#39FF14"))
        } else {
            view.text = getString(R.string.state_fail)
            view.setTextColor(Color.parseColor("#FF3B30"))
        }
    }

    /** 更新运行环境状态卡片。 */
    private fun updateEnvStatus() {
        val rooted = RootUtil.isRooted()
        val overlay = Settings.canDrawOverlays(this)
        val scAvailable = SurfaceControlCapture(this).isAvailable()

        setStateChip(textRootState, rooted)
        setStateChip(textOverlayState, overlay)
        setStateChip(textScState, scAvailable)
        textResolution.text = "${capture.width}x${capture.height}"
    }

    private fun setStateChip(view: TextView, ok: Boolean) {
        if (ok) {
            view.text = getString(R.string.state_ok)
            view.setTextColor(Color.parseColor("#39FF14"))
        } else {
            view.text = getString(R.string.state_fail)
            view.setTextColor(Color.parseColor("#FF3B30"))
        }
    }

    private fun startPipeline() {
        // 1. 悬浮窗权限
        if (!Settings.canDrawOverlays(this)) {
            Toast.makeText(this, R.string.toast_overlay_required, Toast.LENGTH_LONG).show()
            val intent = Intent(
                Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                Uri.parse("package:$packageName")
            )
            startActivity(intent)
            return
        }

        // 2. Root 检测（仅警告，不阻断）
        if (!RootUtil.isRooted()) {
            Toast.makeText(this, R.string.toast_no_root, Toast.LENGTH_LONG).show()
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
        val initialModelKey = availableModels.getOrNull(selectedModelIndex)?.key

        val intent = Intent(this, AimService::class.java).apply {
            putExtra(AimService.EXTRA_RESULT_CODE, resultCode)
            putExtra(AimService.EXTRA_RESULT_DATA, data)
            putExtra(AimService.EXTRA_USE_GPU, useGpu)
            putExtra(AimService.EXTRA_TRIGGER, trigger)
            putExtra(AimService.EXTRA_CAPTURE_MODE, captureMode)
            putExtra(AimService.EXTRA_AUTO_FIRE, autoFire)
            putExtra(AimService.EXTRA_INJECT_BACKEND, injectBackend)
            putExtra(AimService.EXTRA_INITIAL_MODEL_KEY, initialModelKey)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(intent)
        } else {
            startService(intent)
        }
        Toast.makeText(this, R.string.toast_starting, Toast.LENGTH_SHORT).show()
        // 启动后退出主界面，悬浮窗接管控制
        finish()
    }

    override fun onResume() {
        super.onResume()
        updateEnvStatus()
    }
}
