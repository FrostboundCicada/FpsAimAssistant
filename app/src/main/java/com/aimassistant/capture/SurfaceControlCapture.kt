package com.aimassistant.capture

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Rect
import android.os.Build
import android.util.DisplayMetrics
import android.util.Log
import android.view.SurfaceControl
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.nio.ByteBuffer
import java.lang.reflect.Method

/**
 * 屏幕捕获: 基于 SurfaceControl 隐藏 API（Root 设备可用，无需 MediaProjection 授权）。
 *
 * 优势:
 *   - 无需用户授权弹窗，无 MediaProjection 通知
 *   - 延迟更低（直接从 SurfaceFlinger 取帧，跳过 VirtualDisplay 合成）
 *   - 不被部分反作弊检测（MediaProjection 会留下明显痕迹）
 *
 * 实现:
 *   通过反射调用隐藏的 SurfaceControl.screenshot() 方法。
 *   API 29+ 可用 screenshot(Rect, width, height) 重载。
 *   采用轮询模式（目标 FPS 由 targetFps 控制）。
 *
 * 注意: 需要在 Root 设备上运行，部分 ROM 可能限制隐藏 API 访问，
 *       失败时调用方应回退到 MediaProjection。
 */
class SurfaceControlCapture(private val context: Context) : FrameCapture {

    companion object {
        private const val TAG = "SurfaceControlCapture"
    }

    override var onFrame: ((buffer: ByteBuffer, width: Int, height: Int) -> Unit)? = null

    override val width: Int
    override val height: Int

    private var scope: CoroutineScope? = null
    private var pollJob: Job? = null
    private var reuseBuffer: ByteBuffer? = null
    private var reuseBitmap: Bitmap? = null

    // 反射缓存
    private var screenshotMethod: Method? = null
    private var methodResolved = false

    /** 轮询目标帧率，start() 前设置。 */
    var targetFps: Int = 30

    init {
        val dm = context.resources.displayMetrics
        width = dm.widthPixels
        height = dm.heightPixels
    }

    /**
     * 检测 SurfaceControl 隐藏 API 是否可用。
     * 在调用 start() 前先检查，失败则回退到 MediaProjection。
     */
    fun isAvailable(): Boolean {
        resolveScreenshotMethod()
        return screenshotMethod != null
    }

    /**
     * 启动轮询捕获。
     */
    override fun start(): Boolean {
        if (!isAvailable()) {
            Log.e(TAG, "SurfaceControl.screenshot 不可用，请回退到 MediaProjection")
            return false
        }

        // 预分配复用 buffer，避免每帧 GC
        val pixelCount = width * height
        reuseBuffer = ByteBuffer.allocateDirect(pixelCount * 4)
        reuseBitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)

        scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

        val intervalMs = 1000L / targetFps
        pollJob = scope?.launch {
            while (isActive) {
                val t0 = System.currentTimeMillis()
                captureOnce()
                val elapsed = System.currentTimeMillis() - t0
                val sleep = intervalMs - elapsed
                if (sleep > 0) delay(sleep)
            }
        }
        Log.i(TAG, "SurfaceControl 捕获已启动: ${width}x${height} @ ${targetFps}fps")
        return true
    }

    /** 单次截图并回调。 */
    private fun captureOnce() {
        val bitmap = screenshot() ?: return
        val buffer = reuseBuffer ?: return
        buffer.rewind()
        // Bitmap ARGB_8888 经 copyPixelsToBuffer 后字节序为 BGRA（ARM 小端）
        bitmap.copyPixelsToBuffer(buffer)
        // 统一转为 RGBA（与 MediaProjection 路径一致，C++ 用 PIXEL_RGBA2RGB）
        swapBgraToRgba(buffer, width * height)
        buffer.rewind()
        onFrame?.invoke(buffer, width, height)
    }

    /**
     * 调用隐藏的 SurfaceControl.screenshot()。
     * API 29: screenshot(Rect, int, int) -> Bitmap
     */
    private fun screenshot(): Bitmap? {
        val method = screenshotMethod ?: return null
        val fullRect = Rect(0, 0, width, height)
        return try {
            method.invoke(null, fullRect, width, height) as? Bitmap
        } catch (e: Exception) {
            Log.w(TAG, "screenshot 调用失败: ${e.message}")
            null
        }
    }

    /** 反射解析 SurfaceControl.screenshot 隐藏方法。 */
    private fun resolveScreenshotMethod() {
        if (methodResolved) return
        methodResolved = true
        try {
            // 优先找 screenshot(Rect, int, int) 重载（API 17+，API 29 仍可用）
            screenshotMethod = SurfaceControl::class.java.getDeclaredMethod(
                "screenshot",
                Rect::class.java,
                Int::class.javaPrimitiveType,
                Int::class.javaPrimitiveType
            )
            screenshotMethod?.isAccessible = true
            Log.i(TAG, "已解析 SurfaceControl.screenshot(Rect, int, int)")
        } catch (e: NoSuchMethodException) {
            // 尝试 screenshot(int, int) 重载
            try {
                screenshotMethod = SurfaceControl::class.java.getDeclaredMethod(
                    "screenshot",
                    Int::class.javaPrimitiveType,
                    Int::class.javaPrimitiveType
                )
                screenshotMethod?.isAccessible = true
                Log.i(TAG, "已解析 SurfaceControl.screenshot(int, int)")
            } catch (e2: NoSuchMethodException) {
                Log.e(TAG, "无法解析 SurfaceControl.screenshot，隐藏 API 被屏蔽")
            }
        }
    }

    /** 原地交换 BGRA -> RGBA（每像素 4 字节，交换位置 0 和 2）。 */
    private fun swapBgraToRgba(buffer: ByteBuffer, pixelCount: Int) {
        // 直接 ByteBuffer 无 backing array，用 get/put 按索引操作
        var i = 0
        repeat(pixelCount) {
            val b = buffer.get(i)
            val g = buffer.get(i + 1)
            val r = buffer.get(i + 2)
            val a = buffer.get(i + 3)
            buffer.put(i, r)
            buffer.put(i + 1, g)
            buffer.put(i + 2, b)
            buffer.put(i + 3, a)
            i += 4
        }
    }

    override fun stop() {
        pollJob?.cancel()
        scope?.cancel()
        scope = null
        pollJob = null
        reuseBuffer = null
        reuseBitmap?.recycle()
        reuseBitmap = null
        Log.i(TAG, "SurfaceControl 捕获已停止")
    }
}
