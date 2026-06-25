package com.aimassistant.util

import android.content.Context
import org.json.JSONObject

/**
 * 模型管理: 扫描 assets/models/ 下的 NCNN 模型对（.param + .bin），
 * 解析同名 .json 元数据文件（若存在）。
 *
 * 模型文件命名约定:
 *   <modelKey>.param / <modelKey>.bin / <modelKey>.json
 *
 * JSON 字段:
 *   name, input_size, input_blob, output_blob, num_classes, format, bbox_xywh
 */
object ModelManager {

    /** 单个模型的元数据。 */
    data class ModelInfo(
        val key: String,           // 文件前缀（如 "yolov8n_coco80"）
        val displayName: String,   // 显示名
        val paramAsset: String,    // "models/<key>.param"
        val binAsset: String,      // "models/<key>.bin"
        val inputSize: Int,
        val inputBlob: String,
        val outputBlob: String,
        val numClasses: Int,
        val formatInt: Int,        // NativeBridge.FORMAT_DFL_RAW / FORMAT_DECODED
        val bboxXywh: Boolean
    )

    /** 扫描 assets/models/ 目录，返回所有可用模型。 */
    fun scanModels(context: Context): List<ModelInfo> {
        val results = mutableListOf<ModelInfo>()
        val files = try {
            context.assets.list("models") ?: emptyArray()
        } catch (e: Exception) {
            emptyArray()
        }

        // 收集所有 .param 文件
        val paramFiles = files.filter { it.endsWith(".param") }
        for (paramFile in paramFiles) {
            val key = paramFile.removeSuffix(".param")
            val binFile = "$key.bin"
            if (binFile !in files) continue  // 缺 .bin，跳过

            val jsonFile = "$key.json"
            val meta = if (jsonFile in files) {
                parseJson(context, "models/$jsonFile")
            } else null

            val info = ModelInfo(
                key = key,
                displayName = meta?.optString("name", key) ?: key,
                paramAsset = "models/$paramFile",
                binAsset = "models/$binFile",
                inputSize = meta?.optInt("input_size", 640) ?: 640,
                inputBlob = meta?.optString("input_blob", "in0") ?: "in0",
                outputBlob = meta?.optString("output_blob", "out0") ?: "out0",
                numClasses = meta?.optInt("num_classes", 80) ?: 80,
                formatInt = parseFormatStr(meta?.optString("format", "dfl_raw") ?: "dfl_raw"),
                bboxXywh = meta?.optBoolean("bbox_xywh", false) ?: false
            )
            results.add(info)
        }
        // 按显示名排序
        results.sortBy { it.displayName }
        return results
    }

    private fun parseJson(context: Context, assetPath: String): JSONObject? {
        return try {
            context.assets.open(assetPath).bufferedReader().use { JSONObject(it.readText()) }
        } catch (e: Exception) {
            null
        }
    }

    private fun parseFormatStr(s: String): Int {
        return when (s.lowercase()) {
            "decoded", "decoded_nx4plusc" -> 1
            else -> 0  // dfl_raw
        }
    }
}
