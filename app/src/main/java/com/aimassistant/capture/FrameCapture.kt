package com.aimassistant.capture

import java.nio.ByteBuffer

/**
 * 屏幕捕获统一接口，支持在 MediaProjection 与 SurfaceControl 两种后端间切换。
 */
interface FrameCapture {
    /** 帧回调: RGBA 像素 buffer + 宽高。 */
    var onFrame: ((buffer: ByteBuffer, width: Int, height: Int) -> Unit)?

    val width: Int
    val height: Int

    /** 启动捕获，返回是否成功。 */
    fun start(): Boolean

    /** 停止捕获。 */
    fun stop()
}
