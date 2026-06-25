package com.aimassistant.overlay

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.PixelFormat
import android.graphics.Paint
import android.util.Log
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.TextView
import androidx.appcompat.widget.SwitchCompat
import com.aimassistant.util.DriverProbe
import java.util.concurrent.CopyOnWriteArrayList

/**
 * 悬浮窗管理器: 同时管理两个独立窗口
 *
 * 1. [OverlayView] — 全屏透明可视化层（不拦截触摸）:
 *    - 绘制检测框、准星、FPS
 *    - 测试模式: 检测框蓝色半透明填充 + 顶部中心点连绿色线到屏幕顶部中心点
 *
 * 2. [ControlPanelView] — 可拖动控制面板（拦截触摸）:
 *    - 状态显示: FPS / 推理ms / 模型 / 后端 / 驱动状态
 *    - 测试模式开关
 *    - 模型切换按钮（循环切换）
 *    - 退出按钮
 */
class OverlayManager(private val context: Context) {

    companion object {
        private const val TAG = "OverlayManager"
    }

    private val windowManager = context.getSystemService(Context.WINDOW_SERVICE) as WindowManager
    private var overlayView: OverlayView? = null
    private var controlView: ControlPanelView? = null

    /** 与 AimService 通信的回调。 */
    interface Listener {
        /** 测试模式开关被切换。 */
        fun onTestModeToggled(enabled: Boolean)
        /** 请求切换到下一个模型，返回新的模型名（失败返回 null）。 */
        fun onCycleModel(): String?
        /** 运行时参数被调节（置信度/灵敏度/半径/速度）。 */
        fun onParamsChanged(confThresh: Float, sensX: Float, sensY: Float,
                            aimRadius: Float, aimSpeed: Float)
        /** 检测范围被调节（宽/高占屏幕比例 0.0~1.0，是否显示）。 */
        fun onDetectionRangeChanged(wRatio: Float, hRatio: Float, visible: Boolean)
        /** 请求退出服务。 */
        fun onExit()
    }

    var listener: Listener? = null

    /** 一个检测框（屏幕坐标）。 */
    data class Box(val x1: Float, val y1: Float, val x2: Float, val y2: Float,
                   val conf: Float, val classId: Int, val selected: Boolean = false)

    @Volatile private var testMode: Boolean = false
    @Volatile private var currentModelName: String = "—"
    @Volatile private var currentBackendName: String = "—"
    @Volatile private var driverReport: DriverProbe.Report? = null
    // 检测范围（屏幕中心矩形比例 + 是否显示）
    @Volatile private var rangeWRatio: Float = 1.0f
    @Volatile private var rangeHRatio: Float = 1.0f
    @Volatile private var rangeVisible: Boolean = true

    fun show() {
        if (overlayView == null) {
            val view = OverlayView(context)
            val params = WindowManager.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                    WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE or
                    WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.TRANSLUCENT
            )
            params.gravity = Gravity.TOP or Gravity.START
            windowManager.addView(view, params)
            overlayView = view
            // 应用初始检测范围
            view.setDetectionRange(rangeWRatio, rangeHRatio, rangeVisible)
        }
        if (controlView == null) {
            val view = ControlPanelView(context)
            view.listener = object : ControlPanelView.Listener {
                override fun onTestModeToggled(enabled: Boolean) {
                    testMode = enabled
                    overlayView?.setTestMode(enabled)
                    listener?.onTestModeToggled(enabled)
                }
                override fun onCycleModel(): String? {
                    val newModel = listener?.onCycleModel()
                    if (newModel != null) {
                        currentModelName = newModel
                        controlView?.updateModel(newModel)
                    }
                    return newModel
                }
                override fun onParamsChanged(confThresh: Float, sensX: Float, sensY: Float,
                                            aimRadius: Float, aimSpeed: Float) {
                    listener?.onParamsChanged(confThresh, sensX, sensY, aimRadius, aimSpeed)
                }
                override fun onDetectionRangeChanged(wRatio: Float, hRatio: Float, visible: Boolean) {
                    rangeWRatio = wRatio
                    rangeHRatio = hRatio
                    rangeVisible = visible
                    overlayView?.setDetectionRange(wRatio, hRatio, visible)
                    listener?.onDetectionRangeChanged(wRatio, hRatio, visible)
                }
                override fun onExit() {
                    listener?.onExit()
                }
            }
            val params = WindowManager.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                    WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.TRANSLUCENT
            )
            params.gravity = Gravity.TOP or Gravity.START
            params.x = 16
            params.y = 80
            windowManager.addView(view, params)
            controlView = view
            view.updateModel(currentModelName)
            view.updateBackend(currentBackendName)
            view.updateStatus(0f, 0f)
        }
    }

    fun hide() {
        overlayView?.let { windowManager.removeView(it) }
        overlayView = null
        controlView?.let { windowManager.removeView(it) }
        controlView = null
    }

    /** 更新检测框并触发重绘。 */
    fun updateDetections(boxes: List<Box>, fps: Float, inferenceMs: Float) {
        overlayView?.update(boxes, fps, inferenceMs)
        controlView?.updateStatus(fps, inferenceMs)
    }

    /** 外部（如 AimService）设置测试模式状态（不触发回调）。 */
    fun setTestMode(enabled: Boolean) {
        testMode = enabled
        overlayView?.setTestMode(enabled)
        controlView?.setTestModeSwitch(enabled)
    }

    /** 更新当前模型名显示。 */
    fun setModelName(name: String) {
        currentModelName = name
        controlView?.updateModel(name)
    }

    /** 更新当前注入后端名显示。 */
    fun setBackendName(name: String) {
        currentBackendName = name
        controlView?.updateBackend(name)
    }

    /** 更新驱动检测结果。 */
    fun setDriverReport(report: DriverProbe.Report) {
        driverReport = report
        controlView?.updateDriver(report)
    }

    /** 设置检测范围（宽/高占屏幕比例 0.0~1.0）及是否显示。 */
    fun setDetectionRange(wRatio: Float, hRatio: Float, visible: Boolean) {
        rangeWRatio = wRatio.coerceIn(0.1f, 1.0f)
        rangeHRatio = hRatio.coerceIn(0.1f, 1.0f)
        rangeVisible = visible
        overlayView?.setDetectionRange(rangeWRatio, rangeHRatio, visible)
    }
}

// ─────────────────────────────────────────────────────────────
// 可视化层: 全屏透明 View，绘制检测框 + 测试模式上色/连线
// ─────────────────────────────────────────────────────────────
private class OverlayView(context: Context) : View(context) {

    private val boxes = CopyOnWriteArrayList<OverlayManager.Box>()
    @Volatile private var fps: Float = 0f
    @Volatile private var inferenceMs: Float = 0f
    @Volatile private var testMode: Boolean = false
    // 检测范围（屏幕中心矩形，宽/高占屏幕比例 0.0~1.0，1.0=全屏）
    @Volatile private var rangeWRatio: Float = 1.0f
    @Volatile private var rangeHRatio: Float = 1.0f
    @Volatile private var rangeVisible: Boolean = true

    private val boxPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#39FF14")  // 荧光绿
        style = Paint.Style.STROKE
        strokeWidth = 3f
    }
    private val selPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.RED
        style = Paint.Style.STROKE
        strokeWidth = 4f
    }
    private val testFillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        // 蓝色半透明填充（ARGB: 0x88=53% 透明度, 0x0000FF=蓝色）
        color = Color.argb(0x88, 0x00, 0x00, 0xFF)
        style = Paint.Style.FILL
    }
    private val testLinePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        // 绿色直线
        color = Color.parseColor("#00FF00")
        style = Paint.Style.STROKE
        strokeWidth = 2f
        isAntiAlias = true
    }
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE
        textSize = 28f
        isFakeBoldText = true
        setShadowLayer(4f, 0f, 0f, Color.BLACK)
    }
    private val crossPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#FFFF00")
        strokeWidth = 2f
    }
    private val rangePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#00E5FF")  // 荧光青
        style = Paint.Style.STROKE
        strokeWidth = 2f
        // 虚线效果
        pathEffect = android.graphics.DashPathEffect(floatArrayOf(12f, 8f), 0f)
    }
    private val rangeFillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(0x14, 0x00, 0xE5, 0xFF)  // 极淡青色填充
        style = Paint.Style.FILL
    }

    fun setTestMode(enabled: Boolean) {
        testMode = enabled
        postInvalidate()
    }

    /** 设置检测范围（宽/高占屏幕比例 0.0~1.0）及是否显示。 */
    fun setDetectionRange(wRatio: Float, hRatio: Float, visible: Boolean) {
        rangeWRatio = wRatio.coerceIn(0.1f, 1.0f)
        rangeHRatio = hRatio.coerceIn(0.1f, 1.0f)
        rangeVisible = visible
        postInvalidate()
    }

    fun update(newBoxes: List<OverlayManager.Box>, fps: Float, inferenceMs: Float) {
        boxes.clear()
        boxes.addAll(newBoxes)
        this.fps = fps
        this.inferenceMs = inferenceMs
        postInvalidate()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val screenCx = width / 2f
        val screenTopY = 0f

        // 检测范围矩形（屏幕中心，按宽高百分比）
        if (rangeVisible) {
            val rw = width * rangeWRatio
            val rh = height * rangeHRatio
            val left = screenCx - rw / 2f
            val top = (height / 2f) - rh / 2f
            val right = screenCx + rw / 2f
            val bottom = (height / 2f) + rh / 2f
            canvas.drawRect(left, top, right, bottom, rangeFillPaint)
            canvas.drawRect(left, top, right, bottom, rangePaint)
            // 四角标记，便于辨认边界
            val cornerLen = 24f
            canvas.drawLine(left, top, left + cornerLen, top, selPaint)
            canvas.drawLine(left, top, left, top + cornerLen, selPaint)
            canvas.drawLine(right, top, right - cornerLen, top, selPaint)
            canvas.drawLine(right, top, right, top + cornerLen, selPaint)
            canvas.drawLine(left, bottom, left + cornerLen, bottom, selPaint)
            canvas.drawLine(left, bottom, left, bottom - cornerLen, selPaint)
            canvas.drawLine(right, bottom, right - cornerLen, bottom, selPaint)
            canvas.drawLine(right, bottom, right, bottom - cornerLen, selPaint)
        }

        // 检测框
        for (b in boxes) {
            // 测试模式: 蓝色半透明填充（覆盖整个检测框区域）
            if (testMode) {
                canvas.drawRect(b.x1, b.y1, b.x2, b.y2, testFillPaint)
            }
            // 边框
            val p = if (b.selected) selPaint else boxPaint
            canvas.drawRect(b.x1, b.y1, b.x2, b.y2, p)
            // 标签
            canvas.drawText(
                "${b.classId} ${(b.conf * 100).toInt()}%",
                b.x1, b.y1 - 6f, textPaint
            )
            // 测试模式: 模型顶部中心点 -> 屏幕顶部中心点 绿色直线
            if (testMode) {
                val topCx = (b.x1 + b.x2) / 2f
                val topY = b.y1
                canvas.drawLine(topCx, topY, screenCx, screenTopY, testLinePaint)
            }
        }
        // 屏幕中心准星
        val cx = width / 2f
        val cy = height / 2f
        canvas.drawLine(cx - 20f, cy, cx + 20f, cy, crossPaint)
        canvas.drawLine(cx, cy - 20f, cx, cy + 20f, crossPaint)
        // 状态文字
        canvas.drawText(
            "FPS %.0f | %.1fms".format(fps, inferenceMs),
            20f, 40f, textPaint
        )
    }
}

// ─────────────────────────────────────────────────────────────
// 控制面板: 可拖动的悬浮控制窗
// ─────────────────────────────────────────────────────────────
private class ControlPanelView(context: Context) : LinearLayout(context) {

    interface Listener {
        fun onTestModeToggled(enabled: Boolean)
        fun onCycleModel(): String?
        fun onParamsChanged(confThresh: Float, sensX: Float, sensY: Float,
                            aimRadius: Float, aimSpeed: Float)
        fun onDetectionRangeChanged(wRatio: Float, hRatio: Float, visible: Boolean)
        fun onExit()
    }

    var listener: Listener? = null

    private val titleText: TextView
    private val statusText: TextView
    private val driverText: TextView
    private val modelBtn: Button
    private val testSwitch: SwitchCompat
    private val exitBtn: Button
    private val paramsToggle: Button
    private val paramsContainer: LinearLayout
    private val rangeToggle: Button
    private val rangeContainer: LinearLayout
    private val rangeVisibleSwitch: SwitchCompat

    // 当前参数值（与 AimService 默认值一致）
    private var pConfThresh = 0.45f
    private var pSensX = 1.0f
    private var pSensY = 1.0f
    private var pAimRadius = 400f
    private var pAimSpeed = 0.35f
    // 检测范围（宽/高占屏幕比例 0.1~1.0，默认全屏）
    private var pRangeW = 1.0f
    private var pRangeH = 1.0f
    private var pRangeVisible = true

    init {
        orientation = VERTICAL
        setPadding(20, 16, 20, 20)
        // 半透明深色背景
        background = createPanelBg()

        // 标题栏（可拖动）
        titleText = TextView(context).apply {
            text = "AimAssistant 控制"
            setTextColor(Color.WHITE)
            textSize = 15f
            setShadowLayer(4f, 0f, 0f, Color.BLACK)
            setPadding(0, 0, 0, 8)
        }
        addView(titleText)

        // 状态文字
        statusText = TextView(context).apply {
            text = "FPS 0 | 0.0ms\n模型: —\n后端: —"
            setTextColor(Color.parseColor("#E0E0E0"))
            textSize = 12f
            setPadding(0, 4, 0, 4)
        }
        addView(statusText)

        // 驱动状态
        driverText = TextView(context).apply {
            text = "驱动: 未检测"
            setTextColor(Color.parseColor("#FFC107"))
            textSize = 12f
            setPadding(0, 0, 0, 8)
        }
        addView(driverText)

        // 测试模式开关
        val testRow = LinearLayout(context).apply {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        val testLabel = TextView(context).apply {
            text = "测试模式（蓝色上色+绿线）"
            setTextColor(Color.WHITE)
            textSize = 13f
            layoutParams = LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f)
        }
        testSwitch = SwitchCompat(context).apply {
            isChecked = false
            setOnCheckedChangeListener { _, isChecked ->
                listener?.onTestModeToggled(isChecked)
            }
        }
        testRow.addView(testLabel)
        testRow.addView(testSwitch)
        addView(testRow)

        // 参数控制折叠区
        paramsToggle = Button(context).apply {
            text = "▶ 参数调节"
            setOnClickListener {
                val visible = paramsContainer.visibility != View.VISIBLE
                paramsContainer.visibility = if (visible) View.VISIBLE else View.GONE
                text = if (visible) "▼ 参数调节" else "▶ 参数调节"
            }
            textSize = 12f
            setBackgroundColor(Color.TRANSPARENT)
            setTextColor(Color.parseColor("#00E5FF"))
            layoutParams = LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT).apply {
                topMargin = 4
            }
        }
        addView(paramsToggle)

        paramsContainer = LinearLayout(context).apply {
            orientation = VERTICAL
            visibility = View.GONE  // 默认折叠
            setPadding(4, 4, 4, 4)
        }
        // 置信度: 0.10~0.90 (progress 10~90, /100)
        addParamSlider("置信度", 10, 90, (pConfThresh * 100).toInt()) { progress ->
            pConfThresh = progress / 100f
            notifyParamsChanged()
        }
        // X灵敏度: 0.1~3.0 (progress 10~300, /100)
        addParamSlider("X灵敏度", 10, 300, (pSensX * 100).toInt()) { progress ->
            pSensX = progress / 100f
            notifyParamsChanged()
        }
        // Y灵敏度: 0.1~3.0
        addParamSlider("Y灵敏度", 10, 300, (pSensY * 100).toInt()) { progress ->
            pSensY = progress / 100f
            notifyParamsChanged()
        }
        // 瞄准半径: 50~1000
        addParamSlider("瞄准半径", 50, 1000, pAimRadius.toInt()) { progress ->
            pAimRadius = progress.toFloat()
            notifyParamsChanged()
        }
        // 瞄准速度: 0.05~1.0 (progress 5~100, /100)
        addParamSlider("瞄准速度", 5, 100, (pAimSpeed * 100).toInt()) { progress ->
            pAimSpeed = progress / 100f
            notifyParamsChanged()
        }
        addView(paramsContainer)

        // 检测范围折叠区（屏幕中心矩形，可调宽高、可隐藏）
        rangeToggle = Button(context).apply {
            text = "▶ 检测范围"
            setOnClickListener {
                val visible = rangeContainer.visibility != View.VISIBLE
                rangeContainer.visibility = if (visible) View.VISIBLE else View.GONE
                text = if (visible) "▼ 检测范围" else "▶ 检测范围"
            }
            textSize = 12f
            setBackgroundColor(Color.TRANSPARENT)
            setTextColor(Color.parseColor("#00E5FF"))
            layoutParams = LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT).apply {
                topMargin = 4
            }
        }
        addView(rangeToggle)

        rangeContainer = LinearLayout(context).apply {
            orientation = VERTICAL
            visibility = View.GONE  // 默认折叠
            setPadding(4, 4, 4, 4)
        }
        // 显示开关行
        val rangeVisRow = LinearLayout(context).apply {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        val rangeVisLabel = TextView(context).apply {
            text = "屏幕显示范围框"
            setTextColor(Color.WHITE)
            textSize = 12f
            layoutParams = LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f)
        }
        rangeVisibleSwitch = SwitchCompat(context).apply {
            isChecked = pRangeVisible
            setOnCheckedChangeListener { _, isChecked ->
                pRangeVisible = isChecked
                notifyRangeChanged()
            }
        }
        rangeVisRow.addView(rangeVisLabel)
        rangeVisRow.addView(rangeVisibleSwitch)
        rangeContainer.addView(rangeVisRow)
        // 宽度比例: 10%~100% (progress 10~100, /100)
        addRangeSlider("范围宽度", 10, 100, (pRangeW * 100).toInt()) { progress ->
            pRangeW = progress / 100f
            notifyRangeChanged()
        }
        // 高度比例: 10%~100%
        addRangeSlider("范围高度", 10, 100, (pRangeH * 100).toInt()) { progress ->
            pRangeH = progress / 100f
            notifyRangeChanged()
        }
        addView(rangeContainer)

        // 模型切换按钮
        modelBtn = Button(context).apply {
            text = "模型: —"
            setOnClickListener {
                val newModel = listener?.onCycleModel()
                if (newModel != null) {
                    text = "模型: $newModel"
                }
            }
            textSize = 12f
            layoutParams = LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT).apply {
                topMargin = 8
            }
        }
        addView(modelBtn)

        // 退出按钮
        exitBtn = Button(context).apply {
            text = "退出"
            setOnClickListener { listener?.onExit() }
            textSize = 12f
            layoutParams = LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT).apply {
                topMargin = 8
            }
        }
        addView(exitBtn)

        // 标题栏拖动
        setupDrag(titleText)
        setupDrag(statusText)
    }

    /** 添加一个参数滑块行: 标签 + 数值 + SeekBar。 */
    private fun addParamSlider(
        label: String, min: Int, max: Int, default: Int,
        onChange: (progress: Int) -> Unit
    ) {
        val row = LinearLayout(context).apply {
            orientation = VERTICAL
            layoutParams = LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT).apply {
                topMargin = 6
            }
        }
        val labelRow = LinearLayout(context).apply {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        val labelText = TextView(context).apply {
            text = label
            setTextColor(Color.parseColor("#E0E0E0"))
            textSize = 11f
            layoutParams = LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f)
        }
        val valueText = TextView(context).apply {
            text = formatSliderValue(label, default)
            setTextColor(Color.parseColor("#00E5FF"))
            textSize = 11f
            setTypeface(typeface, android.graphics.Typeface.BOLD)
        }
        labelRow.addView(labelText)
        labelRow.addView(valueText)
        row.addView(labelRow)

        val seekBar = SeekBar(context).apply {
            this.max = max - min
            progress = default - min
            // 面板宽度有限，限制 SeekBar 宽度
            layoutParams = LayoutParams(240, LayoutParams.WRAP_CONTENT)
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(sb: SeekBar?, p: Int, fromUser: Boolean) {
                    if (fromUser) {
                        val realProgress = p + min
                        valueText.text = formatSliderValue(label, realProgress)
                        onChange(realProgress)
                    }
                }
                override fun onStartTrackingTouch(sb: SeekBar?) {}
                override fun onStopTrackingTouch(sb: SeekBar?) {}
            })
        }
        row.addView(seekBar)
        paramsContainer.addView(row)
    }

    /** 根据参数类型格式化显示值。 */
    private fun formatSliderValue(label: String, progress: Int): String {
        return when (label) {
            "置信度" -> "%.2f".format(progress / 100f)
            "X灵敏度", "Y灵敏度", "瞄准速度" -> "%.2f".format(progress / 100f)
            "瞄准半径" -> "$progress"
            "范围宽度", "范围高度" -> "${progress}%"
            else -> progress.toString()
        }
    }

    private fun notifyParamsChanged() {
        listener?.onParamsChanged(pConfThresh, pSensX, pSensY, pAimRadius, pAimSpeed)
    }

    /** 添加检测范围滑块（结构与 addParamSlider 相同，但加入 rangeContainer）。 */
    private fun addRangeSlider(
        label: String, min: Int, max: Int, default: Int,
        onChange: (progress: Int) -> Unit
    ) {
        val row = LinearLayout(context).apply {
            orientation = VERTICAL
            layoutParams = LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT).apply {
                topMargin = 6
            }
        }
        val labelRow = LinearLayout(context).apply {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        val labelText = TextView(context).apply {
            text = label
            setTextColor(Color.parseColor("#E0E0E0"))
            textSize = 11f
            layoutParams = LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f)
        }
        val valueText = TextView(context).apply {
            text = formatSliderValue(label, default)
            setTextColor(Color.parseColor("#00E5FF"))
            textSize = 11f
            setTypeface(typeface, android.graphics.Typeface.BOLD)
        }
        labelRow.addView(labelText)
        labelRow.addView(valueText)
        row.addView(labelRow)

        val seekBar = SeekBar(context).apply {
            this.max = max - min
            progress = default - min
            layoutParams = LayoutParams(240, LayoutParams.WRAP_CONTENT)
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(sb: SeekBar?, p: Int, fromUser: Boolean) {
                    if (fromUser) {
                        val realProgress = p + min
                        valueText.text = formatSliderValue(label, realProgress)
                        onChange(realProgress)
                    }
                }
                override fun onStartTrackingTouch(sb: SeekBar?) {}
                override fun onStopTrackingTouch(sb: SeekBar?) {}
            })
        }
        row.addView(seekBar)
        rangeContainer.addView(row)
    }

    private fun notifyRangeChanged() {
        listener?.onDetectionRangeChanged(pRangeW, pRangeH, pRangeVisible)
    }

    private fun createPanelBg(): android.graphics.drawable.Drawable {
        val paint = android.graphics.Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.argb(0xCC, 0x1A, 0x1A, 0x2E)  // 深蓝灰，80% 不透明
        }
        return object : android.graphics.drawable.Drawable() {
            override fun draw(canvas: Canvas) {
                val r = bounds
                canvas.drawRoundRect(
                    r.left.toFloat(), r.top.toFloat(),
                    r.right.toFloat(), r.bottom.toFloat(),
                    24f, 24f, paint
                )
            }
            override fun setAlpha(alpha: Int) {}
            override fun setColorFilter(colorFilter: android.graphics.ColorFilter?) {}
            @Suppress("OverrideDeprecatedMigration", "DeprecatedCallableAddedReplaceWith")
            override fun getOpacity(): Int = PixelFormat.TRANSLUCENT
        }
    }

    private fun setupDrag(handle: View) {
        handle.setOnTouchListener { v, e ->
            val wm = context.getSystemService(Context.WINDOW_SERVICE) as WindowManager
            when (e.action) {
                MotionEvent.ACTION_DOWN -> {
                    v.tag = floatArrayOf(e.rawX, e.rawY)
                    true
                }
                MotionEvent.ACTION_MOVE -> {
                    val start = v.tag as? FloatArray ?: return@setOnTouchListener false
                    val dx = e.rawX - start[0]
                    val dy = e.rawY - start[1]
                    val lp = (layoutParams as? WindowManager.LayoutParams)
                    if (lp != null) {
                        lp.x += dx.toInt()
                        lp.y += dy.toInt()
                        try {
                            wm.updateViewLayout(this@ControlPanelView, lp)
                        } catch (ex: Exception) {
                            Log.w("ControlPanel", "updateViewLayout: ${ex.message}")
                        }
                        v.tag = floatArrayOf(e.rawX, e.rawY)
                    }
                    true
                }
                else -> false
            }
        }
    }

    fun updateStatus(fps: Float, inferenceMs: Float) {
        statusText.text = "FPS ${fps.toInt()} | ${"%.1f".format(inferenceMs)}ms\n模型: ${modelBtn.text.substringAfter("模型: ")}\n后端: ${currentBackendText}"
        postInvalidate()
    }

    private var currentBackendText: String = "—"
    fun updateBackend(name: String) {
        currentBackendText = name
        statusText.text = "FPS 0 | 0.0ms\n模型: ${modelBtn.text.substringAfter("模型: ")}\n后端: $name"
    }

    fun updateModel(name: String) {
        modelBtn.text = "模型: $name"
    }

    fun updateDriver(report: DriverProbe.Report) {
        val sb = StringBuilder("驱动: ")
        if (report.anyAvailable) {
            sb.append("✓ ")
            val parts = mutableListOf<String>()
            if (report.kernelDriver.available) parts.add("内核")
            if (report.twtDriver.available) parts.add("TwT")
            if (report.gyroscope.available) parts.add("陀螺仪")
            if (report.uinput.available) parts.add("uinput")
            sb.append(parts.joinToString("/"))
            sb.append("\n  ${report.kernelDriver.detail}")
        } else {
            sb.append("✗ 全部不可用")
            sb.append("\n  无 aim_touch/zero/qx/rt/TwT 节点")
        }
        driverText.text = sb.toString()
    }

    fun setTestModeSwitch(enabled: Boolean) {
        testSwitch.setOnCheckedChangeListener(null)
        testSwitch.isChecked = enabled
        testSwitch.setOnCheckedChangeListener { _, isChecked ->
            listener?.onTestModeToggled(isChecked)
        }
    }
}
