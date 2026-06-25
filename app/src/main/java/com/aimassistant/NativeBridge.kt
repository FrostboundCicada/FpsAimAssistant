package com.aimassistant

import java.nio.ByteBuffer

/**
 * JNI 桥接: 连接 Kotlin 层与 C++ 推理/注入层。
 * 对应 jni_bridge.cpp 中的 extern "C" 函数。
 */
object NativeBridge {

    init {
        System.loadLibrary("aimassistant")
    }

    /** 模型输出格式: 0=DFL_RAW(需解码), 1=DECODED(已解码 xyxy+sigmoid) */
    const val FORMAT_DFL_RAW = 0
    const val FORMAT_DECODED = 1

    /**
     * 初始化: 加载 NCNN 模型 + 创建注入设备。
     *
     * @param paramPath      .param 文件路径（已拷贝到内部存储）
     * @param binPath        .bin 文件路径
     * @param useGpu         true=Vulkan, false=CPU(ARM Neon)
     * @param numThreads     CPU 线程数
     * @param screenW        屏幕宽
     * @param screenH        屏幕高
     * @param preferredBackend 优先后端: 0=uinput, 1=内核驱动, 2=陀螺仪, 3=TwT, -1=自动探测
     * @param inputBlob      模型输入 blob 名（如 "in0" / "images"）
     * @param outputBlob     模型输出 blob 名（如 "out0" / "Identity"）
     * @param inputSize      模型输入尺寸（如 640 / 256）
     * @param numClasses     类别数
     * @param formatInt      输出格式: FORMAT_DFL_RAW 或 FORMAT_DECODED
     * @param bboxXywh       true=bbox 是 [cx,cy,w,h]（仅 DECODED 格式生效）
     * @return true 表示成功
     */
    external fun nativeInit(
        paramPath: String, binPath: String,
        useGpu: Boolean, numThreads: Int,
        screenW: Int, screenH: Int,
        preferredBackend: Int = -1,
        inputBlob: String = "in0",
        outputBlob: String = "out0",
        inputSize: Int = 640,
        numClasses: Int = 80,
        formatInt: Int = FORMAT_DFL_RAW,
        bboxXywh: Boolean = false
    ): Boolean

    /**
     * 运行时切换模型（不重启注入器，仅替换 YOLO 推理器）。
     * 参数含义同 [nativeInit] 中模型相关参数，外加 displayName。
     * @return true 表示切换成功
     */
    external fun nativeLoadModel(
        paramPath: String, binPath: String,
        inputBlob: String = "in0",
        outputBlob: String = "out0",
        inputSize: Int = 640,
        numClasses: Int = 80,
        formatInt: Int = FORMAT_DFL_RAW,
        bboxXywh: Boolean = false,
        displayName: String = "model"
    ): Boolean

    /**
     * 查询当前已加载模型信息。
     * @return String[6] = {name, input_size, num_classes, format, input_blob, output_blob}
     */
    external fun nativeGetLoadedModelInfo(): Array<String>

    /**
     * 对一帧 RGBA 数据进行检测。
     * @param buffer 直接 ByteBuffer，存放 RGBA 像素
     * @param width  原图宽
     * @param height 原图高
     * @return float[N*6] = {x1,y1,x2,y2,conf,class}，无目标时长度为 0
     */
    external fun nativeDetect(
        buffer: ByteBuffer, width: Int, height: Int,
        confThresh: Float, nmsThresh: Float
    ): FloatArray

    /** 触摸注入一次瞄准（按下->贝塞尔移动->抬起）。仅触摸后端有效。 */
    external fun nativeAimOnce(aimX: Float, aimY: Float)

    /** 陀螺仪瞄准: 持续微调（每帧调用）。仅陀螺仪后端有效。 */
    external fun nativeGyroAim(dx: Float, dy: Float)

    /** 获取当前注入后端名称。 */
    external fun nativeGetBackendName(): String

    /** 设置瞄准参数。 */
    external fun nativeSetConfig(
        aimRadius: Float, aimSpeed: Float, leadFactor: Float,
        pipelineMs: Float, headshot: Boolean, trigger: Boolean
    )

    /** 最近一次推理耗时（毫秒）。 */
    external fun nativeGetInferenceMs(): Float

    /** 注入设备是否就绪。 */
    external fun nativeInjectorReady(): Boolean

    /** 释放所有原生资源。 */
    external fun nativeShutdown()
}
