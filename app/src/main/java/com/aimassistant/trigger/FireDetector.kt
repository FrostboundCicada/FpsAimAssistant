package com.aimassistant.trigger

import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.delay
import java.io.File
import java.io.FileInputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * 开火检测器: 自动触发瞄准注入。
 *
 * 双重检测策略:
 *   1. 输入事件监听（主）: 读取 /dev/input/eventX，检测用户手指按下开火按钮区域。
 *      Root 设备可读 input 设备节点。多数 FPS 手游的开火键在右下角固定区域。
 *   2. 亮度尖峰检测（备选）: 分析屏幕中心区域的亮度突变（枪口火焰）。
 *      当输入监听不可用或作为补充时启用。
 *
 * 检测到开火时回调 [onFire]，由 AimService 触发 nativeAimOnce。
 *
 * @param screenW 屏幕宽（像素）
 * @param screenH 屏幕高（像素）
 * @param fireZone 开火按钮判定区域（屏幕坐标），默认右下角
 */
class FireDetector(
    private val screenW: Int,
    private val screenH: Int,
    private var fireZone: Rect = defaultFireZone(screenW, screenH)
) {

    companion object {
        private const val TAG = "FireDetector"
        private const val INPUT_DIR = "/dev/input"

        /** 默认开火键区域: 右下角 1/4 区域（多数 FPS 手游布局）。 */
        fun defaultFireZone(w: Int, h: Int): Rect {
            // 右下角，约占屏幕右半下 40%
            val left = w / 2
            val top = (h * 0.6f).toInt()
            return Rect(left, top, w, h)
        }
    }

    /** 简化矩形（避免依赖 android.graphics.Rect 在纯逻辑层）。 */
    data class Rect(val left: Int, val top: Int, val right: Int, val bottom: Int) {
        fun contains(x: Int, y: Int): Boolean =
            x in left..right && y in top..bottom
    }

    /** 开火回调: 参数为检测到的开火点坐标。 */
    var onFire: (() -> Unit)? = null

    /** 是否启用亮度尖峰检测（备选/补充）。 */
    var brightnessDetectionEnabled = false

    // ── 输入事件监听 ──────────────────────────────────────
    private var scope: CoroutineScope? = null
    private var inputJob: Job? = null
    private var inputDevices: List<String> = emptyList()

    // input_event 结构体大小（64 位内核: 24 字节）
    // struct input_event { struct timeval time; __u16 type; __u16 code; __s32 value; }
    // timeval 在 64 位为 16 字节，总计 24 字节
    private val EVENT_SIZE = 24
    private val EVENT_TYPE_OFFSET = 16
    private val EVENT_CODE_OFFSET = 18
    private val EVENT_VALUE_OFFSET = 20

    // 触摸状态
    private var touchActive = false
    private var touchX = 0
    private var touchY = 0

    // 开火去抖
    private var lastFireTime = 0L
    private val fireCooldownMs = 80L  // 连续开火最小间隔

    // ── 亮度检测 ──────────────────────────────────────────
    private var prevBrightness = 0f
    private val brightnessThreshold = 0.15f  // 亮度增量阈值

    /**
     * 启动开火检测。
     * @param autoAimEnabled 是否启用自动瞄准（false 时仅检测不回调）
     */
    fun start(autoAimEnabled: Boolean) {
        scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
        inputDevices = findTouchDevices()
        Log.i(TAG, "检测到触摸设备: $inputDevices, 开火区域: $fireZone, 自动瞄准: $autoAimEnabled")

        if (inputDevices.isEmpty()) {
            Log.w(TAG, "未找到可读的触摸设备，将依赖亮度检测")
            brightnessDetectionEnabled = true
            return
        }

        // 为每个触摸设备启动监听协程
        for (dev in inputDevices) {
            inputJob = scope?.launch { listenInputDevice(dev, autoAimEnabled) }
        }
    }

    /** 停止检测。 */
    fun stop() {
        inputJob?.cancel()
        scope?.cancel()
        scope = null
        inputJob = null
        touchActive = false
    }

    /** 更新开火按钮区域（运行时可调）。 */
    fun setFireZone(zone: Rect) {
        fireZone = zone
    }

    /**
     * 亮度尖峰检测: 由 ScreenCapture 每帧调用。
     * 分析屏幕中心区域的平均亮度，检测枪口火焰导致的亮度突变。
     */
    fun feedFrame(buffer: ByteBuffer, w: Int, h: Int) {
        if (!brightnessDetectionEnabled) return

        // 采样中心区域（准星附近）的平均亮度
        val cx = w / 2
        val cy = h / 2
        val radius = 60
        var sum = 0L
        var count = 0
        for (y in (cy - radius) until (cy + radius) step 4) {
            for (x in (cx - radius) until (cx + radius) step 4) {
                if (x < 0 || x >= w || y < 0 || y >= h) continue
                val idx = (y * w + x) * 4
                if (idx + 2 >= buffer.capacity()) continue
                // RGBA: R=idx, G=idx+1, B=idx+2
                val r = buffer.get(idx).toInt() and 0xFF
                val g = buffer.get(idx + 1).toInt() and 0xFF
                val b = buffer.get(idx + 2).toInt() and 0xFF
                // 亮度 = 0.299R + 0.587G + 0.114B
                sum += (0.299 * r + 0.587 * g + 0.114 * b).toLong()
                count++
            }
        }
        if (count == 0) return
        val brightness = sum.toFloat() / (count * 255f)

        // 检测亮度尖峰
        val delta = brightness - prevBrightness
        prevBrightness = brightness

        if (delta > brightnessThreshold) {
            triggerFire()
        }
    }

    // ── 内部实现 ──────────────────────────────────────────

    /** 查找 /dev/input 下可读的触摸设备节点。 */
    private fun findTouchDevices(): List<String> {
        val dir = File(INPUT_DIR)
        if (!dir.exists()) return emptyList()
        return dir.listFiles()
            ?.filter { it.name.startsWith("event") }
            ?.filter { it.canRead() }
            ?.map { it.absolutePath }
            ?: emptyList()
    }

    /**
     * 监听单个 input 设备，解析触摸事件。
     * Root 设备下 /dev/input/eventX 可读，但需 root 权限。
     */
    private suspend fun listenInputDevice(devicePath: String, autoAimEnabled: Boolean) {
        val buf = ByteBuffer.allocate(EVENT_SIZE).order(ByteOrder.nativeOrder())
        val fis = try {
            FileInputStream(devicePath)
        } catch (e: Exception) {
            Log.w(TAG, "无法打开 $devicePath: ${e.message}")
            return
        }

        Log.i(TAG, "开始监听 $devicePath")
        try {
            while (scope?.isActive == true) {
                val n = fis.read(buf.array())
                if (n < EVENT_SIZE) continue
                buf.rewind()
                val type = buf.getShort(EVENT_TYPE_OFFSET).toInt() and 0xFFFF
                val code = buf.getShort(EVENT_CODE_OFFSET).toInt() and 0xFFFF
                val value = buf.getInt(EVENT_VALUE_OFFSET)

                // EV_ABS = 0x03, EV_SYN = 0x00
                when (type) {
                    0x03 -> { // EV_ABS
                        when (code) {
                            0x35 -> touchX = value  // ABS_MT_POSITION_X
                            0x36 -> touchY = value  // ABS_MT_POSITION_Y
                            0x39 -> { // ABS_MT_TRACKING_ID
                                if (value >= 0) {
                                    touchActive = true
                                } else {
                                    // 手指抬起: 检查是否在开火区域释放
                                    if (touchActive && fireZone.contains(touchX, touchY)) {
                                        if (autoAimEnabled) triggerFire()
                                    }
                                    touchActive = false
                                }
                            }
                        }
                    }
                    0x01 -> { // EV_KEY, BTN_TOUCH = 0x14a
                        if (code == 0x14a) {
                            if (value == 1) {
                                touchActive = true
                            } else {
                                if (touchActive && fireZone.contains(touchX, touchY)) {
                                    if (autoAimEnabled) triggerFire()
                                }
                                touchActive = false
                            }
                        }
                    }
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "监听 $devicePath 异常: ${e.message}")
        } finally {
            fis.close()
        }
    }

    /** 触发开火回调（带去抖）。 */
    private fun triggerFire() {
        val now = System.currentTimeMillis()
        if (now - lastFireTime < fireCooldownMs) return
        lastFireTime = now
        onFire?.invoke()
    }
}
