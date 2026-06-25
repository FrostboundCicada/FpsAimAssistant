package com.aimassistant.overlay

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.PixelFormat
import android.os.Build
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import java.util.concurrent.CopyOnWriteArrayList

/**
 * 悬浮窗覆盖层: 显示检测框、瞄准点、FPS。
 *
 * 使用 WindowManager + TYPE_APPLICATION_OVERLAY，独立于目标应用，
 * 透明穿透（不拦截触摸），仅用于可视化。
 */
class OverlayManager(private val context: Context) {

    private val windowManager = context.getSystemService(Context.WINDOW_SERVICE) as WindowManager
    private var overlayView: OverlayView? = null

    fun show() {
        if (overlayView != null) return
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
    }

    fun hide() {
        overlayView?.let {
            windowManager.removeView(it)
        }
        overlayView = null
    }

    /** 更新检测框并触发重绘。 */
    fun updateDetections(boxes: List<Box>, fps: Float, inferenceMs: Float) {
        overlayView?.update(boxes, fps, inferenceMs)
    }

    /** 一个检测框（屏幕坐标）。 */
    data class Box(val x1: Float, val y1: Float, val x2: Float, val y2: Float,
                   val conf: Float, val classId: Int, val selected: Boolean = false)
}

/** 实际绘制的 View。 */
private class OverlayView(context: Context) : View(context) {

    private val boxes = CopyOnWriteArrayList<OverlayManager.Box>()
    @Volatile private var fps: Float = 0f
    @Volatile private var inferenceMs: Float = 0f

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

    fun update(newBoxes: List<OverlayManager.Box>, fps: Float, inferenceMs: Float) {
        boxes.clear()
        boxes.addAll(newBoxes)
        this.fps = fps
        this.inferenceMs = inferenceMs
        postInvalidate()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        // 检测框
        for (b in boxes) {
            val p = if (b.selected) selPaint else boxPaint
            canvas.drawRect(b.x1, b.y1, b.x2, b.y2, p)
            canvas.drawText(
                "${b.classId} ${(b.conf * 100).toInt()}%",
                b.x1, b.y1 - 6f, textPaint
            )
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
