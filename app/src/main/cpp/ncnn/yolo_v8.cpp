// yolo_v8.cpp — NCNN YOLOv8n 推理实现
//
// 处理流程:
//   1. RGBA -> NCNN Mat（归一化到 [0,1]，RGB 通道顺序）
//   2. letterbox 缩放到 640x640
//   3. 前向推理（CPU ARM Neon 或 Vulkan）
//   4. 解码输出 [1,84,8400] -> 检测框
//   5. NMS 去重
//   6. 坐标映射回原图尺度
#include "yolo_v8.h"

#include "ncnn/net.h"
#include "ncnn/mat.h"
#include "ncnn/gpu.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <vector>

#include <android/log.h>

#define LOG_TAG "yolo_v8"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace aimbot {

// YOLOv8 类别数（COCO 80 类）。如自训练模型类别数不同请修改。
static const int kNumClasses = 80;

// 计算两框 IoU
static float iou(const Detection& a, const Detection& b) {
    float xx1 = std::max(a.x1, b.x1);
    float yy1 = std::max(a.y1, b.y1);
    float xx2 = std::min(a.x2, b.x2);
    float yy2 = std::min(a.y2, b.y2);
    float w = std::max(0.0f, xx2 - xx1);
    float h = std::max(0.0f, yy2 - yy1);
    float inter = w * h;
    float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    return inter / (area_a + area_b - inter + 1e-6f);
}

// NMS
static void nms(std::vector<Detection>& dets, float thresh) {
    std::sort(dets.begin(), dets.end(),
              [](const Detection& a, const Detection& b) { return a.confidence > b.confidence; });
    std::vector<int> suppressed(dets.size(), 0);
    for (size_t i = 0; i < dets.size(); ++i) {
        if (suppressed[i]) continue;
        for (size_t j = i + 1; j < dets.size(); ++j) {
            if (suppressed[j]) continue;
            if (dets[i].class_id != dets[j].class_id) continue;
            if (iou(dets[i], dets[j]) > thresh) suppressed[j] = 1;
        }
    }
    std::vector<Detection> out;
    for (size_t i = 0; i < dets.size(); ++i) {
        if (!suppressed[i]) out.push_back(dets[i]);
    }
    dets = std::move(out);
}

YoloV8::YoloV8() : net_(nullptr), last_ms_(0.f), input_size_(640) {}

YoloV8::~YoloV8() {
    unload();
}

bool YoloV8::isLoaded() const {
    return net_ != nullptr;
}

float YoloV8::lastInferenceMs() const {
    return last_ms_;
}

bool YoloV8::load(const std::string& param_path, const std::string& bin_path,
                  bool use_gpu, int num_threads) {
    unload();
    net_ = new ncnn::Net();

    // 后端选择
    if (use_gpu) {
        if (ncnn::get_gpu_count() > 0) {
            net_->opt.use_vulkan_compute = true;
            LOGI("NCNN 后端: Vulkan GPU");
        } else {
            LOGW("Vulkan 不可用，回退到 CPU");
            net_->opt.use_vulkan_compute = false;
        }
    } else {
        net_->opt.use_vulkan_compute = false;
        LOGI("NCNN 后端: CPU (ARM Neon, %d 线程)", num_threads);
    }

    net_->opt.num_threads = num_threads;
    net_->opt.lightmode = true;
    net_->opt.use_fp16_packed = true;
    net_->opt.use_fp16_storage = true;
    net_->opt.use_fp16_arithmetic = true;  // FP16 加速
    net_->opt.use_packing_layout = true;

#if NCNN_INT8
    net_->opt.use_int8_inference = true;
#endif

    // 加载模型
    int rp = net_->load_param(param_path.c_str());
    int rb = net_->load_model(bin_path.c_str());
    if (rp != 0 || rb != 0) {
        LOGE("模型加载失败 param=%d bin=%d (%s / %s)", rp, rb,
             param_path.c_str(), bin_path.c_str());
        delete net_;
        net_ = nullptr;
        return false;
    }
    LOGI("模型加载成功: %s", param_path.c_str());
    return true;
}

void YoloV8::unload() {
    if (net_) {
        delete net_;
        net_ = nullptr;
    }
}

std::vector<Detection> YoloV8::detect(const uint8_t* rgba, int img_w, int img_h,
                                      float conf_thresh, float nms_thresh) {
    std::vector<Detection> result;
    if (!net_ || !rgba || img_w <= 0 || img_h <= 0) return result;

    auto t0 = std::chrono::steady_clock::now();

    const int target = input_size_;  // 640

    // letterbox 计算
    float scale = std::min((float)target / img_w, (float)target / img_h);
    int new_w = (int)(img_w * scale);
    int new_h = (int)(img_h * scale);
    int pad_w = (target - new_w) / 2;
    int pad_h = (target - new_h) / 2;

    // RGBA -> RGB，缩放到 new_w x new_h（保持长宽比）
    ncnn::Mat resized = ncnn::Mat::from_pixels_resize(rgba, ncnn::Mat::PIXEL_RGBA2RGB,
                                                      img_w, img_h, new_w, new_h);

    // letterbox: 用灰色(0.5)填充到 target x target
    ncnn::Mat in;
    ncnn::copy_make_border(resized, in, pad_h, target - new_h - pad_h,
                           pad_w, target - new_w - pad_w,
                           ncnn::BORDER_CONSTANT, 0.5f);

    // 归一化到 [0,1]
    const float norm[3] = {1.f / 255.f, 1.f / 255.f, 1.f / 255.f};
    in.substract_mean_normalize(nullptr, norm);

    // 前向推理
    ncnn::Extractor ex = net_->create_extractor();
    ex.set_light_mode(true);
    ex.input("in0", in);

    ncnn::Mat out;
    ex.extract("out0", out);

    auto t1 = std::chrono::steady_clock::now();
    last_ms_ = std::chrono::duration<float, std::milli>(t1 - t0).count();

    // 解码输出
    // YOLOv8 输出维度: [1, 4 + num_classes, num_anchors] 或 [1, num_anchors, 4+num_classes]
    // 这里处理常见的 [1, 84, 8400] 布局（NCNN 通常为 w=8400, h=84）
    if (out.empty()) {
        LOGW("推理输出为空");
        return result;
    }

    const int num_classes = kNumClasses;          // 80
    const int num_elem    = 4 + num_classes;        // 84
    const float* data = (const float*)out.data;

    // 兼容两种 NCNN 输出布局:
    //   A) [1, 84, 8400]  -> out.w=8400(anchors), out.h=84(elem)
    //   B) [8400, 84]     -> out.w=84(elem),     out.h=8400(anchors)
    int num_anchors, anchor_stride, elem_stride;
    if (out.w == num_elem) {
        // Layout B
        num_anchors   = out.h;
        anchor_stride = out.w;   // 每个 anchor 一行
        elem_stride   = 1;
    } else {
        // Layout A
        num_anchors   = out.w;
        anchor_stride = 1;
        elem_stride   = out.w;  // 元素间隔 = anchor 数
    }

    // letterbox 逆变换参数
    float gain = scale;
    float pad_x = pad_w;
    float pad_y = pad_h;

    for (int i = 0; i < num_anchors; ++i) {
        const float* p = data + i * anchor_stride;

        float cx = p[0 * elem_stride];
        float cy = p[1 * elem_stride];
        float w  = p[2 * elem_stride];
        float h  = p[3 * elem_stride];

        // 找最大类别置信度
        float max_score = 0.f;
        int max_cls = 0;
        for (int c = 0; c < num_classes; ++c) {
            float s = p[(4 + c) * elem_stride];
            if (s > max_score) { max_score = s; max_cls = c; }
        }
        if (max_score < conf_thresh) continue;

        // 坐标从 640 尺度映射回原图
        float x1 = (cx - w * 0.5f - pad_x) / gain;
        float y1 = (cy - h * 0.5f - pad_y) / gain;
        float x2 = (cx + w * 0.5f - pad_x) / gain;
        float y2 = (cy + h * 0.5f - pad_y) / gain;

        // 裁剪到原图范围
        x1 = std::max(0.f, std::min(x1, (float)img_w - 1));
        y1 = std::max(0.f, std::min(y1, (float)img_h - 1));
        x2 = std::max(0.f, std::min(x2, (float)img_w - 1));
        y2 = std::max(0.f, std::min(y2, (float)img_h - 1));

        Detection d;
        d.x1 = x1; d.y1 = y1; d.x2 = x2; d.y2 = y2;
        d.confidence = max_score;
        d.class_id = max_cls;
        result.push_back(d);
    }

    nms(result, nms_thresh);

    LOGI("推理 %.1f ms, 检测到 %zu 个目标", last_ms_, result.size());
    return result;
}

} // namespace aimbot
