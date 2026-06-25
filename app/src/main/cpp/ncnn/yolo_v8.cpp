// yolo_v8.cpp — NCNN YOLOv8 推理实现（多模型/多输出格式）
//
// 处理流程:
//   1. RGBA -> NCNN Mat（归一化到 [0,1]，RGB 通道顺序）
//   2. letterbox 缩放到 input_size x input_size
//   3. 前向推理（CPU ARM Neon 或 Vulkan）
//   4. 根据 meta.format 走两条解码路径:
//        - DFL_RAW:          [N, 64+C] → DFL 解码 → xyxy + sigmoid 类别
//        - DECODED_Nx4plusC: [N, 4+C]  → 直接 xyxy + 已 sigmoid 类别
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
#include <tuple>

#include <android/log.h>

#define LOG_TAG "yolo_v8"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace aimbot {

// YOLOv8 DFL reg_max（每个坐标的分布桶数）
static constexpr int kRegMax = 16;
// YOLOv8 三个尺度的 stride
static constexpr int kStrides[3] = {8, 16, 32};

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
            if (iou(dets[i], dets[j]) > thresh) suppressed[j] = 1;
        }
    }
    std::vector<Detection> out;
    for (size_t i = 0; i < dets.size(); ++i) {
        if (!suppressed[i]) out.push_back(dets[i]);
    }
    dets = std::move(out);
}

static inline float sigmoidf(float x) {
    return 1.f / (1.f + expf(-x));
}

YoloV8::YoloV8() : net_(nullptr), last_ms_(0.f), total_anchors_(0) {}

YoloV8::~YoloV8() {
    unload();
}

bool YoloV8::isLoaded() const {
    return net_ != nullptr;
}

float YoloV8::lastInferenceMs() const {
    return last_ms_;
}

void YoloV8::buildAnchorPoints() {
    anchors_.clear();
    total_anchors_ = 0;
    const int S = meta_.input_size;
    for (int s : kStrides) {
        int grid = S / s;
        for (int y = 0; y < grid; ++y) {
            for (int x = 0; x < grid; ++x) {
                // anchor point 在 grid cell 中心（input_size 尺度下）
                anchors_.emplace_back((x + 0.5f) * s, (y + 0.5f) * s, (float)s);
                total_anchors_++;
            }
        }
    }
    LOGI("anchor points: %d (input=%d)", total_anchors_, S);
}

bool YoloV8::load(const std::string& param_path, const std::string& bin_path,
                  const ModelMeta& meta,
                  bool use_gpu, int num_threads) {
    unload();
    meta_ = meta;
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
    LOGI("模型加载成功: %s (input=%d, classes=%d, format=%d, blob=%s->%s)",
         meta_.name.c_str(), meta_.input_size, meta_.num_classes, (int)meta_.format,
         meta_.input_blob.c_str(), meta_.output_blob.c_str());

    if (meta_.format == OutputFormat::DFL_RAW) {
        buildAnchorPoints();
    }
    return true;
}

void YoloV8::unload() {
    if (net_) {
        delete net_;
        net_ = nullptr;
    }
    anchors_.clear();
    total_anchors_ = 0;
}

// DFL 解码单个 anchor
//   data: 指向 [64 + num_classes] 数据
//   out_ltrb: 输出 4 维 [left, top, right, bottom]
//   out_cls_scores: 输出 num_classes 维（已 sigmoid）
void YoloV8::decodeDflAnchor(const float* data, float* out_ltrb, float* out_cls_scores) {
    const int reg_total = 4 * kRegMax;  // 64
    // 对每个坐标方向（4 个）做 DFL: softmax over 16 buckets，加权求和
    for (int k = 0; k < 4; ++k) {
        const float* p = data + k * kRegMax;
        float maxv = p[0];
        for (int j = 1; j < kRegMax; ++j) if (p[j] > maxv) maxv = p[j];
        float sum = 0.f;
        float expv[kRegMax];
        for (int j = 0; j < kRegMax; ++j) {
            expv[j] = expf(p[j] - maxv);
            sum += expv[j];
        }
        float val = 0.f;
        for (int j = 0; j < kRegMax; ++j) {
            val += (float)j * expv[j] / sum;
        }
        out_ltrb[k] = val;
    }
    // 类别分数: sigmoid
    const float* cls_data = data + reg_total;
    for (int c = 0; c < meta_.num_classes; ++c) {
        out_cls_scores[c] = sigmoidf(cls_data[c]);
    }
}

std::vector<Detection> YoloV8::detect(const uint8_t* rgba, int img_w, int img_h,
                                      float conf_thresh, float nms_thresh) {
    std::vector<Detection> result;
    if (!net_ || !rgba || img_w <= 0 || img_h <= 0) return result;

    auto t0 = std::chrono::steady_clock::now();

    const int target = meta_.input_size;

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
    ex.input(meta_.input_blob.c_str(), in);

    ncnn::Mat out;
    ex.extract(meta_.output_blob.c_str(), out);

    auto t1 = std::chrono::steady_clock::now();
    last_ms_ = std::chrono::duration<float, std::milli>(t1 - t0).count();

    if (out.empty()) {
        LOGW("推理输出为空");
        return result;
    }

    // NCNN Mat 维度: c=1, h=rows, w=cols（或 c=h=w=1, c = total）
    // YOLOv8 输出常见两种布局:
    //   A) [1, num_elem, num_anchors]  → out.w = num_anchors, out.h = num_elem
    //   B) [num_anchors, num_elem]     → out.w = num_elem,    out.h = num_anchors
    //   C) [num_elem, num_anchors]     → 同 A
    const int num_classes = meta_.num_classes;
    const int reg_total   = (meta_.format == OutputFormat::DFL_RAW) ? (4 * kRegMax) : 4;
    const int num_elem    = reg_total + num_classes;

    int num_anchors, anchor_stride, elem_stride;
    if (out.w == num_elem) {
        // Layout B: 每行一个 anchor
        num_anchors   = out.h;
        anchor_stride = out.w;
        elem_stride   = 1;
    } else {
        // Layout A: 每 anchor 占一列
        num_anchors   = out.w;
        anchor_stride = 1;
        elem_stride   = out.w;
    }
    const float* data = (const float*)out.data;

    // letterbox 逆变换参数
    float gain = scale;
    float pad_x = pad_w;
    float pad_y = pad_h;

    // 校验 anchor 数量（仅 DFL_RAW 模式需要 anchor 表）
    if (meta_.format == OutputFormat::DFL_RAW) {
        if (total_anchors_ != num_anchors) {
            LOGW("anchor 数不匹配: 模型输出 %d，预生成 %d（可能 input_size 错误）",
                 num_anchors, total_anchors_);
        }
    }

    // 临时 buffer 用于 DFL 解码的 cls 分数
    std::vector<float> cls_buf(num_classes);

    for (int i = 0; i < num_anchors; ++i) {
        const float* p = data + i * anchor_stride;

        float x1, y1, x2, y2;
        float max_score = 0.f;
        int   max_cls = 0;

        if (meta_.format == OutputFormat::DFL_RAW) {
            // DFL 解码
            float ltrb[4];
            decodeDflAnchor(p, ltrb, cls_buf.data());
            for (int c = 0; c < num_classes; ++c) {
                if (cls_buf[c] > max_score) { max_score = cls_buf[c]; max_cls = c; }
            }
            // anchor point (input_size 尺度下)
            float ax = 0, ay = 0;
            if (i < total_anchors_) {
                ax = std::get<0>(anchors_[i]);
                ay = std::get<1>(anchors_[i]);
            } else {
                // 兜底: 无法确定 anchor 位置，跳过
                continue;
            }
            // ltrb 是相对 anchor point 的距离（input_size 尺度下）
            float bx1 = ax - ltrb[0];
            float by1 = ay - ltrb[1];
            float bx2 = ax + ltrb[2];
            float by2 = ay + ltrb[3];
            // 映射回原图
            x1 = (bx1 - pad_x) / gain;
            y1 = (by1 - pad_y) / gain;
            x2 = (bx2 - pad_x) / gain;
            y2 = (by2 - pad_y) / gain;
        } else {
            // DECODED_Nx4plusC: 前 4 维是 bbox
            float v0 = p[0 * elem_stride];
            float v1 = p[1 * elem_stride];
            float v2 = p[2 * elem_stride];
            float v3 = p[3 * elem_stride];
            if (meta_.bbox_xywh) {
                // xywh -> xyxy
                float cx = v0, cy = v1, w = v2, h = v3;
                x1 = cx - w * 0.5f;
                y1 = cy - h * 0.5f;
                x2 = cx + w * 0.5f;
                y2 = cy + h * 0.5f;
            } else {
                x1 = v0; y1 = v1; x2 = v2; y2 = v3;
            }
            // 类别分数（已 sigmoid，取最大）
            for (int c = 0; c < num_classes; ++c) {
                float s = p[(4 + c) * elem_stride];
                // 兼容: 部分模型可能未在网内 sigmoid，这里再 sigmoid 一次（幂等性容错）
                // 但若已 sigmoid，再 sigmoid 会失真——所以仅当 s 超出 [0,1] 时才 sigmoid
                if (s < 0.f || s > 1.f) s = sigmoidf(s);
                if (s > max_score) { max_score = s; max_cls = c; }
            }
            // 映射回原图（bbox 是 input_size 尺度下的像素坐标）
            x1 = (x1 - pad_x) / gain;
            y1 = (y1 - pad_y) / gain;
            x2 = (x2 - pad_x) / gain;
            y2 = (y2 - pad_y) / gain;
        }

        if (max_score < conf_thresh) continue;

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
