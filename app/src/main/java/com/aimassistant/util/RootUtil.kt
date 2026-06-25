package com.aimassistant.util

import android.content.Context
import java.io.File

/**
 * Root 相关工具: 检测 su 可用性，并将 assets 中的模型文件拷贝到内部存储。
 */
object RootUtil {

    /** 检测设备是否已 Root（su 可执行）。 */
    fun isRooted(): Boolean {
        return try {
            val p = Runtime.getRuntime().exec(arrayOf("which", "su"))
            val ok = p.waitFor() == 0
            ok && p.inputStream.bufferedReader().readText().isNotEmpty()
        } catch (e: Exception) {
            false
        }
    }

    /**
     * 将 assets 中的文件拷贝到内部存储并返回目标路径。
     * NCNN 需要文件系统路径（无法直接读 assets）。
     */
    fun copyAssetToFile(context: Context, assetName: String): File {
        val out = File(context.filesDir, assetName)
        if (out.exists() && out.length() > 0) return out
        out.parentFile?.mkdirs()
        context.assets.open(assetName).use { input ->
            out.outputStream().use { output -> input.copyTo(output) }
        }
        return out
    }
}
