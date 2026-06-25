// jni_bridge.cpp — JNI 入口，连接 Kotlin 层与 C++ 推理/注入层
//
// 对应 Kotlin 类: com.aimassistant.NativeBridge
//
// 模型管理:
//   - nativeInit 时加载初始模型（带元数据）
//   - nativeLoadModel 运行时切换模型（不重启注入器）
//   - nativeGetLoadedModelInfo 查询当前模型信息
#include <jni.h>
#include <android/log.h>
#include <unistd.h>

#include <memory>
#include <vector>
#include <string>
#include <mutex>

#include "ncnn/yolo_v8.h"
#include "input/touch_injector.h"
#include "aimbot/aimbot_controller.h"

#define LOG_TAG "jni_bridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using namespace aimbot;

// 全局单例
namespace {
struct Pipeline {
    std::unique_ptr<YoloV8>           yolo;
    std::unique_ptr<AimbotController>  controller;
    ModelMeta                          currentMeta;
    bool                               useGpu   = false;
    int                                numThreads = 4;
    std::mutex                         loadMutex;  // 切换模型时上锁，避免与推理并发
};
Pipeline g_pipe;
}

extern "C" {

// ─────────────────────────────────────────────────────────────
// 初始化: 加载初始模型 + 创建注入设备
//   preferredBackend: 0=uinput, 1=内核驱动, 2=陀螺仪, 3=TwT驱动, -1=自动探测
//   modelKey: 模型文件前缀（如 "yolov8n_coco80"），对应 assets/models/<modelKey>.param/.bin/.json
// ─────────────────────────────────────────────────────────────
JNIEXPORT jboolean JNICALL
Java_com_aimassistant_NativeBridge_nativeInit(
        JNIEnv* env, jclass,
        jstring jParamPath, jstring jBinPath,
        jboolean useGpu, jint numThreads,
        jint screenW, jint screenH, jint preferredBackend,
        jstring jInputBlob, jstring jOutputBlob,
        jint inputSize, jint numClasses, jint formatInt, jboolean bboxXywh) {

    const char* param_path = env->GetStringUTFChars(jParamPath, nullptr);
    const char* bin_path   = env->GetStringUTFChars(jBinPath, nullptr);
    const char* input_blob = jInputBlob ? env->GetStringUTFChars(jInputBlob, nullptr) : "in0";
    const char* output_blob= jOutputBlob ? env->GetStringUTFChars(jOutputBlob, nullptr) : "out0";

    std::lock_guard<std::mutex> lk(g_pipe.loadMutex);

    if (!g_pipe.yolo) g_pipe.yolo = std::make_unique<YoloV8>();
    if (!g_pipe.controller) g_pipe.controller = std::make_unique<AimbotController>();

    // 构造元数据
    ModelMeta meta;
    meta.name         = "init-model";
    meta.input_size   = inputSize > 0 ? inputSize : 640;
    meta.input_blob   = input_blob;
    meta.output_blob  = output_blob;
    meta.num_classes  = numClasses > 0 ? numClasses : 80;
    meta.format       = (formatInt >= 0 && formatInt <= 1) ? (OutputFormat)formatInt : OutputFormat::DFL_RAW;
    meta.bbox_xywh    = bboxXywh == JNI_TRUE;
    g_pipe.currentMeta = meta;
    g_pipe.useGpu      = useGpu == JNI_TRUE;
    g_pipe.numThreads  = numThreads > 0 ? numThreads : 4;

    bool ok = g_pipe.yolo->load(param_path, bin_path, meta, g_pipe.useGpu, g_pipe.numThreads);
    if (ok) {
        // 后端选择: preferredBackend < 0 表示自动探测（TwT->内核驱动->陀螺仪->uinput）
        InjectBackend preferred = InjectBackend::NONE;
        switch (preferredBackend) {
        case 0:  preferred = InjectBackend::UINPUT; break;
        case 1:  preferred = InjectBackend::KERNEL_DRIVER; break;
        case 2:  preferred = InjectBackend::GYROSCOPE; break;
        case 3:  preferred = InjectBackend::TWT_DRIVER; break;
        default: preferred = InjectBackend::NONE; break;
        }
        // 若注入器已就绪（之前已初始化），保持不变；否则初始化
        if (!g_pipe.controller->injector().isReady()) {
            InjectBackend used = g_pipe.controller->initInjector(screenW, screenH, preferred);
            ok = (used != InjectBackend::NONE);
            if (ok) {
                LOGI("注入后端: %s", g_pipe.controller->injector().backendName().c_str());
            }
        }
    }

    env->ReleaseStringUTFChars(jParamPath, param_path);
    env->ReleaseStringUTFChars(jBinPath, bin_path);
    if (jInputBlob) env->ReleaseStringUTFChars(jInputBlob, input_blob);
    if (jOutputBlob) env->ReleaseStringUTFChars(jOutputBlob, output_blob);

    LOGI("nativeInit: %s", ok ? "成功" : "失败");
    return ok ? JNI_TRUE : JNI_FALSE;
}

// ─────────────────────────────────────────────────────────────
// 运行时切换模型（不重启注入器，仅替换 YOLO）
// 返回 true 表示切换成功
// ─────────────────────────────────────────────────────────────
JNIEXPORT jboolean JNICALL
Java_com_aimassistant_NativeBridge_nativeLoadModel(
        JNIEnv* env, jclass,
        jstring jParamPath, jstring jBinPath,
        jstring jInputBlob, jstring jOutputBlob,
        jint inputSize, jint numClasses, jint formatInt, jboolean bboxXywh,
        jstring jDisplayName) {

    const char* param_path = env->GetStringUTFChars(jParamPath, nullptr);
    const char* bin_path   = env->GetStringUTFChars(jBinPath, nullptr);
    const char* input_blob = jInputBlob ? env->GetStringUTFChars(jInputBlob, nullptr) : "in0";
    const char* output_blob= jOutputBlob ? env->GetStringUTFChars(jOutputBlob, nullptr) : "out0";
    const char* disp_name  = jDisplayName ? env->GetStringUTFChars(jDisplayName, nullptr) : "model";

    ModelMeta meta;
    meta.name         = disp_name;
    meta.input_size   = inputSize > 0 ? inputSize : 640;
    meta.input_blob   = input_blob;
    meta.output_blob  = output_blob;
    meta.num_classes  = numClasses > 0 ? numClasses : 80;
    meta.format       = (formatInt >= 0 && formatInt <= 1) ? (OutputFormat)formatInt : OutputFormat::DFL_RAW;
    meta.bbox_xywh    = bboxXywh == JNI_TRUE;

    bool ok;
    {
        std::lock_guard<std::mutex> lk(g_pipe.loadMutex);
        // 若 YoloV8 对象不存在，先创建
        if (!g_pipe.yolo) g_pipe.yolo = std::make_unique<YoloV8>();
        ok = g_pipe.yolo->load(param_path, bin_path, meta, g_pipe.useGpu, g_pipe.numThreads);
        if (ok) {
            g_pipe.currentMeta = meta;
            LOGI("模型切换成功: %s", disp_name);
        } else {
            LOGE("模型切换失败: %s", param_path);
        }
    }

    env->ReleaseStringUTFChars(jParamPath, param_path);
    env->ReleaseStringUTFChars(jBinPath, bin_path);
    if (jInputBlob) env->ReleaseStringUTFChars(jInputBlob, input_blob);
    if (jOutputBlob) env->ReleaseStringUTFChars(jOutputBlob, output_blob);
    if (jDisplayName) env->ReleaseStringUTFChars(jDisplayName, disp_name);

    return ok ? JNI_TRUE : JNI_FALSE;
}

// ─────────────────────────────────────────────────────────────
// 查询当前已加载模型的信息
// 返回 String[]: {name, input_size, num_classes, format, input_blob, output_blob}
// ─────────────────────────────────────────────────────────────
JNIEXPORT jobjectArray JNICALL
Java_com_aimassistant_NativeBridge_nativeGetLoadedModelInfo(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lk(g_pipe.loadMutex);
    const ModelMeta& m = g_pipe.currentMeta;
    const char* format_str = (m.format == OutputFormat::DFL_RAW) ? "dfl_raw" : "decoded";

    jclass strClass = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray(6, strClass, nullptr);
    env->SetObjectArrayElement(arr, 0, env->NewStringUTF(m.name.c_str()));
    env->SetObjectArrayElement(arr, 1, env->NewStringUTF(std::to_string(m.input_size).c_str()));
    env->SetObjectArrayElement(arr, 2, env->NewStringUTF(std::to_string(m.num_classes).c_str()));
    env->SetObjectArrayElement(arr, 3, env->NewStringUTF(format_str));
    env->SetObjectArrayElement(arr, 4, env->NewStringUTF(m.input_blob.c_str()));
    env->SetObjectArrayElement(arr, 5, env->NewStringUTF(m.output_blob.c_str()));
    return arr;
}

// ─────────────────────────────────────────────────────────────
// 推理: 输入 RGBA ByteBuffer，返回检测框数组
// 返回: float[N*6] = {x1,y1,x2,y2,conf,class} 重复 N 次
// ─────────────────────────────────────────────────────────────
JNIEXPORT jfloatArray JNICALL
Java_com_aimassistant_NativeBridge_nativeDetect(
        JNIEnv* env, jclass,
        jobject jBuffer, jint width, jint height,
        jfloat confThresh, jfloat nmsThresh) {

    std::lock_guard<std::mutex> lk(g_pipe.loadMutex);
    if (!g_pipe.yolo) return env->NewFloatArray(0);

    uint8_t* rgba = (uint8_t*)env->GetDirectBufferAddress(jBuffer);
    if (!rgba) return env->NewFloatArray(0);

    std::vector<Detection> dets =
        g_pipe.yolo->detect(rgba, width, height, confThresh, nmsThresh);

    // 同时更新跟踪器（aimOnce 时复用）
    if (g_pipe.controller) {
        float ax = width * 0.5f, ay = height * 0.5f;
        g_pipe.controller->processFrame(dets, ax, ay, 16.6f);
    }

    int n = (int)dets.size();
    jfloatArray out = env->NewFloatArray(n * 6);
    if (n == 0) return out;

    std::vector<float> flat(n * 6);
    for (int i = 0; i < n; ++i) {
        flat[i * 6 + 0] = dets[i].x1;
        flat[i * 6 + 1] = dets[i].y1;
        flat[i * 6 + 2] = dets[i].x2;
        flat[i * 6 + 3] = dets[i].y2;
        flat[i * 6 + 4] = dets[i].confidence;
        flat[i * 6 + 5] = (float)dets[i].class_id;
    }
    env->SetFloatArrayRegion(out, 0, n * 6, flat.data());
    return out;
}

// ─────────────────────────────────────────────────────────────
// 触发一次瞄准注入（触摸后端）
// ─────────────────────────────────────────────────────────────
JNIEXPORT void JNICALL
Java_com_aimassistant_NativeBridge_nativeAimOnce(
        JNIEnv*, jclass, jfloat aimX, jfloat aimY) {
    if (g_pipe.controller) {
        g_pipe.controller->aimOnce(aimX, aimY);
    }
}

// ─────────────────────────────────────────────────────────────
// 陀螺仪瞄准: 持续微调（陀螺仪后端，每帧调用）
//   dx, dy: 目标相对准星的像素偏移
// ─────────────────────────────────────────────────────────────
JNIEXPORT void JNICALL
Java_com_aimassistant_NativeBridge_nativeGyroAim(
        JNIEnv*, jclass, jfloat dx, jfloat dy) {
    if (g_pipe.controller) {
        g_pipe.controller->gyroAim(dx, dy);
    }
}

// ─────────────────────────────────────────────────────────────
// 获取当前注入后端名称
// ─────────────────────────────────────────────────────────────
JNIEXPORT jstring JNICALL
Java_com_aimassistant_NativeBridge_nativeGetBackendName(JNIEnv* env, jclass) {
    std::string name = g_pipe.controller ? g_pipe.controller->injector().backendName() : "无";
    return env->NewStringUTF(name.c_str());
}

// ─────────────────────────────────────────────────────────────
// 配置: 瞄准半径、速度、提前量、头部模式、触发开关
// ─────────────────────────────────────────────────────────────
JNIEXPORT void JNICALL
Java_com_aimassistant_NativeBridge_nativeSetConfig(
        JNIEnv*, jclass,
        jfloat aimRadius, jfloat aimSpeed, jfloat leadFactor,
        jfloat pipelineMs, jboolean headshot, jboolean trigger) {
    if (!g_pipe.controller) return;
    AimConfig& c = g_pipe.controller->config();
    c.aim_radius      = aimRadius;
    c.aim_speed       = aimSpeed;
    c.lead_factor     = leadFactor;
    c.pipeline_ms     = pipelineMs;
    c.headshot_mode   = headshot;
    c.trigger_enabled = trigger;
}

// ─────────────────────────────────────────────────────────────
// 获取最近一次推理耗时
// ─────────────────────────────────────────────────────────────
JNIEXPORT jfloat JNICALL
Java_com_aimassistant_NativeBridge_nativeGetInferenceMs(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lk(g_pipe.loadMutex);
    if (g_pipe.yolo) return g_pipe.yolo->lastInferenceMs();
    return 0.f;
}

// ─────────────────────────────────────────────────────────────
// 检查注入设备是否就绪
// ─────────────────────────────────────────────────────────────
JNIEXPORT jboolean JNICALL
Java_com_aimassistant_NativeBridge_nativeInjectorReady(JNIEnv*, jclass) {
    if (g_pipe.controller) return g_pipe.controller->injector().isReady() ? JNI_TRUE : JNI_FALSE;
    return JNI_FALSE;
}

// ─────────────────────────────────────────────────────────────
// 释放
// ─────────────────────────────────────────────────────────────
JNIEXPORT void JNICALL
Java_com_aimassistant_NativeBridge_nativeShutdown(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lk(g_pipe.loadMutex);
    if (g_pipe.controller) g_pipe.controller->shutdown();
    g_pipe.yolo.reset();
    g_pipe.controller.reset();
    LOGI("nativeShutdown: 已释放");
}

} // extern "C"
