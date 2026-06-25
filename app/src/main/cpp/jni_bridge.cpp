// jni_bridge.cpp — JNI 入口，连接 Kotlin 层与 C++ 推理/注入层
//
// 对应 Kotlin 类: com.aimassistant.NativeBridge
#include <jni.h>
#include <android/log.h>
#include <unistd.h>

#include <memory>
#include <vector>
#include <string>

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
};
Pipeline g_pipe;
}

extern "C" {

// ─────────────────────────────────────────────────────────────
// 初始化: 加载模型 + 创建注入设备
//   preferredBackend: 0=uinput, 1=内核驱动, 2=陀螺仪, 3=TwT驱动, -1=自动探测
// ─────────────────────────────────────────────────────────────
JNIEXPORT jboolean JNICALL
Java_com_aimassistant_NativeBridge_nativeInit(
        JNIEnv* env, jclass,
        jstring jParamPath, jstring jBinPath,
        jboolean useGpu, jint numThreads,
        jint screenW, jint screenH, jint preferredBackend) {

    const char* param_path = env->GetStringUTFChars(jParamPath, nullptr);
    const char* bin_path   = env->GetStringUTFChars(jBinPath, nullptr);

    if (!g_pipe.yolo) g_pipe.yolo = std::make_unique<YoloV8>();
    if (!g_pipe.controller) g_pipe.controller = std::make_unique<AimbotController>();

    bool ok = g_pipe.yolo->load(param_path, bin_path, useGpu, numThreads);
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
        InjectBackend used = g_pipe.controller->initInjector(screenW, screenH, preferred);
        ok = (used != InjectBackend::NONE);
        if (ok) {
            LOGI("注入后端: %s", g_pipe.controller->injector().backendName().c_str());
        }
    }

    env->ReleaseStringUTFChars(jParamPath, param_path);
    env->ReleaseStringUTFChars(jBinPath, bin_path);

    LOGI("nativeInit: %s", ok ? "成功" : "失败");
    return ok ? JNI_TRUE : JNI_FALSE;
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

    if (!g_pipe.yolo) return env->NewFloatArray(0);

    uint8_t* rgba = (uint8_t*)env->GetDirectBufferAddress(jBuffer);
    if (!rgba) return env->NewFloatArray(0);

    std::vector<Detection> dets =
        g_pipe.yolo->detect(rgba, width, height, confThresh, nmsThresh);

    // 同时更新跟踪器（aimOnce 时复用）
    if (g_pipe.controller) {
        // 用屏幕中心作为默认瞄准点（实际由 Kotlin 传入更准，这里仅更新跟踪）
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
    if (g_pipe.controller) g_pipe.controller->shutdown();
    g_pipe.yolo.reset();
    g_pipe.controller.reset();
    LOGI("nativeShutdown: 已释放");
}

} // extern "C"
