package com.aimassistant.util

import android.util.Log
import java.io.File

/**
 * 驱动检测: 检查触摸注入后端的可用性。
 *
 * 检测项:
 *   1. 内核驱动节点 (aim_touch / zero_touch / qx_touch / rt_touch / ovo_touch / hakutaku)
 *   2. TwT 驱动 (anon_inode fd / syscall reboot hook)
 *   3. 陀螺仪设备 (/dev/input/event* 支持 ABS_RX/RY/RZ，且非触摸屏)
 *   4. uinput (/dev/uinput 可写)
 *   5. 已加载内核模块 (/proc/modules)
 *
 * 注意: 完整的"链接成功"需要 nativeInjectorReady()，本类只做用户态可探测的部分。
 *      TwT 的真正可用性需在 native 层 syscall 后才能确认。
 */
object DriverProbe {

    private const val TAG = "DriverProbe"

    /** 内核驱动设备节点列表（与 touch_injector.cpp openKnownDriver 一致）。 */
    private val KERNEL_DRIVER_NODES = listOf(
        "/dev/aim_touch", "/dev/zero_touch", "/dev/zero",
        "/dev/qx_touch", "/dev/qx",
        "/dev/rt_touch", "/dev/rt",
        "/dev/ovo_touch", "/dev/hakutaku"
    )

    /** 已知内核模块名。 */
    private val KNOWN_MODULES = listOf(
        "aim_touch", "zero_touch", "zero", "qx_touch", "qx",
        "rt_touch", "rt", "ovo_touch", "hakutaku", "TwT_driver"
    )

    /** 单个驱动后端的检测结果。 */
    data class BackendStatus(
        val name: String,           // 后端显示名
        val available: Boolean,     // 节点/模块是否可用
        val detail: String,         // 详情（节点路径 / 模块名 / 错误）
        val linked: Boolean         // 是否已成功链接（仅 native 注入器就绪后为 true）
    )

    /** 完整检测结果。 */
    data class Report(
        val kernelDriver: BackendStatus,
        val twtDriver: BackendStatus,
        val gyroscope: BackendStatus,
        val uinput: BackendStatus,
        val loadedModules: List<String>,  // 命中的内核模块名
        val anyAvailable: Boolean
    )

    /** 执行完整检测。 */
    fun probe(): Report {
        val kernel = probeKernelDriver()
        val twt = probeTwtDriver()
        val gyro = probeGyroscope()
        val uinput = probeUinput()
        val modules = probeLoadedModules()

        // 若某后端节点存在但未在 /proc/modules 命中，仍认为 available=true（节点存在即可用）
        val any = kernel.available || twt.available || gyro.available || uinput.available

        return Report(
            kernelDriver = kernel,
            twtDriver = twt,
            gyroscope = gyro,
            uinput = uinput,
            loadedModules = modules,
            anyAvailable = any
        )
    }

    /** 检测内核驱动节点。 */
    private fun probeKernelDriver(): BackendStatus {
        for (path in KERNEL_DRIVER_NODES) {
            val f = File(path)
            if (f.exists()) {
                val writable = f.canWrite()
                // 即使不可写，root 下可通过 su 提权后写入；这里仅判断存在
                return BackendStatus(
                    name = "内核驱动",
                    available = true,
                    detail = "$path (存在, ${if (writable) "可写" else "需 Root 提权"})",
                    linked = false  // 真正链接状态由 native 层注入器判断
                )
            }
        }
        return BackendStatus(
            name = "内核驱动",
            available = false,
            detail = "未找到 aim_touch/zero/qx/rt 等节点",
            linked = false
        )
    }

    /** 检测 TwT 驱动（用户态可探测的部分）。 */
    private fun probeTwtDriver(): BackendStatus {
        // 方式1: /proc/self/fd 中查找 anon_inode TwT_driver
        val fdDir = File("/proc/self/fd")
        if (fdDir.exists() && fdDir.isDirectory) {
            try {
                fdDir.listFiles()?.forEach { fdFile ->
                    try {
                        val link = android.system.Os.readlink(fdFile.absolutePath)
                        if (link.contains("TwT_driver") && link.contains("anon_inode")) {
                            return BackendStatus(
                                name = "TwT 驱动",
                                available = true,
                                detail = "fd=${fdFile.name} ($link)",
                                linked = false
                            )
                        }
                    } catch (_: Exception) {}
                }
            } catch (e: Exception) {
                Log.w(TAG, "扫描 /proc/self/fd 失败: ${e.message}")
            }
        }

        // 方式2: /proc/modules 中是否有 TwT_driver
        val hasModule = probeLoadedModules().contains("TwT_driver")
        if (hasModule) {
            return BackendStatus(
                name = "TwT 驱动",
                available = true,
                detail = "内核模块 TwT_driver 已加载（需 syscall 验证）",
                linked = false
            )
        }

        return BackendStatus(
            name = "TwT 驱动",
            available = false,
            detail = "未检测到 TwT_driver（fd 或模块）",
            linked = false
        )
    }

    /** 检测陀螺仪设备（用户态粗略探测: 找 /dev/input/event* 文件存在）。 */
    private fun probeGyroscope(): BackendStatus {
        val inputDir = File("/dev/input")
        if (!inputDir.exists()) {
            return BackendStatus("陀螺仪", false, "/dev/input 不存在", false)
        }
        val events = inputDir.listFiles { f -> f.name.startsWith("event") } ?: emptyArray()
        if (events.isEmpty()) {
            return BackendStatus("陀螺仪", false, "/dev/input 下无 event 设备", false)
        }
        // 精确探测需要 ioctl EVIOCGBIT，这里仅在 native 层完成；
        // 用户态给出粗略计数即可
        return BackendStatus(
            name = "陀螺仪",
            available = true,  // 有 event 设备就视为可能可用，精确判断由 native 完成
            detail = "${events.size} 个 event 设备（精确能力需 native ioctl 探测）",
            linked = false
        )
    }

    /** 检测 uinput。 */
    private fun probeUinput(): BackendStatus {
        val f = File("/dev/uinput")
        return when {
            !f.exists() -> BackendStatus("uinput", false, "/dev/uinput 不存在", false)
            f.canWrite() -> BackendStatus("uinput", true, "/dev/uinput 可写", false)
            else -> BackendStatus("uinput", true, "/dev/uinput 存在但需 Root 提权", false)
        }
    }

    /** 读取 /proc/modules，返回命中的已知模块名。 */
    private fun probeLoadedModules(): List<String> {
        val modules = mutableListOf<String>()
        try {
            File("/proc/modules").bufferedReader().useLines { lines ->
                lines.forEach { line ->
                    val name = line.substringBefore(' ').trim()
                    if (name in KNOWN_MODULES) modules.add(name)
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "读取 /proc/modules 失败: ${e.message}")
        }
        return modules
    }
}
