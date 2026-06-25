package com.aimassistant.capture

import android.content.Context
import android.content.Intent
import android.graphics.PixelFormat
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.Image
import android.media.ImageReader
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.util.DisplayMetrics
import android.view.Surface
import java.nio.ByteBuffer

/**
 * 屏幕捕获: 基于 MediaProjection + ImageReader。
 *
 * 流程:
 *   1. MainActivity 通过 createPermissionIntent() 申请权限
 *   2. 拿到 resultCode + data 后调用 start()
 *   3. 每帧通过 ImageReader 回调获取 RGBA 数据
 *
 * 性能: ImageReader 使用 RGBA_8888，直接喂给 NCNN，避免格式转换。
 * 延迟主要来自系统合成，通常 < 16ms。
 */
class ScreenCapture(private val context: Context) : FrameCapture {

    private var projection: MediaProjection? = null
    private var imageReader: ImageReader? = null
    private var virtualDisplay: VirtualDisplay? = null

    override var onFrame: ((buffer: ByteBuffer, width: Int, height: Int) -> Unit)? = null

    override val width: Int
    override val height: Int

    init {
        val dm = context.resources.displayMetrics
        // 使用真实分辨率（MediaProjection 捕获物理屏幕）
        width = dm.widthPixels
        height = dm.heightPixels
    }

    /** 创建权限申请 Intent（在 Activity 中 startActivityForResult）。 */
    fun createPermissionIntent(): Intent {
        val mgr = context.getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        return mgr.createScreenCaptureIntent()
    }

    /**
     * 启动捕获（需先通过 [setPermissionResult] 设置授权结果）。
     */
    override fun start(): Boolean {
        val (resultCode, data) = permissionResult ?: return false
        val mgr = context.getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        projection = mgr.getMediaProjection(resultCode, data) ?: return false

        // ImageReader: RGBA_8888，分辨率与屏幕一致
        imageReader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 2)
        imageReader?.setOnImageAvailableListener({ reader ->
            val image = reader.acquireLatestImage() ?: return@setOnImageAvailableListener
            try {
                onFrame?.invoke(imageToBuffer(image), width, height)
            } finally {
                image.close()
            }
        }, null)

        virtualDisplay = projection?.createVirtualDisplay(
            "AimCapture", width, height, context.resources.displayMetrics.densityDpi,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
            imageReader?.surface, null, null
        )
        return virtualDisplay != null
    }

    /** 缓存 MediaProjection 授权结果，供 start() 使用。 */
    private var permissionResult: Pair<Int, Intent>? = null
    fun setPermissionResult(resultCode: Int, data: Intent) {
        permissionResult = resultCode to data
    }

    /** 将 Image 转为连续 RGBA ByteBuffer（处理行 stride padding）。 */
    private fun imageToBuffer(image: Image): ByteBuffer {
        val plane = image.planes[0]
        val src = plane.buffer
        val rowStride = plane.rowStride
        val pixelStride = plane.pixelStride  // RGBA_8888 应为 4

        // 若无 padding，直接返回
        if (rowStride == width * pixelStride) {
            return src
        }
        // 有 padding 时拷贝到紧凑 buffer
        val compact = ByteBuffer.allocateDirect(width * height * pixelStride)
        var pos = 0
        for (row in 0 until height) {
            src.position(row * rowStride)
            src.limit(row * rowStride + width * pixelStride)
            compact.put(src)
            src.limit(src.capacity())
        }
        compact.flip()
        return compact
    }

    fun stop() {
        virtualDisplay?.release()
        virtualDisplay = null
        imageReader?.setOnImageAvailableListener(null, null)
        imageReader?.close()
        imageReader = null
        projection?.stop()
        projection = null
    }
}
