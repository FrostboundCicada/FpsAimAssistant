// yolo_v8.h — NCNN 部署 YOLOv8 目标检测（多模型/多输出格式）
//
// 支持两种 NCNN 输出格式:
//   1. DFL_RAW         — 输出 [N, 64 + num_classes]，需自行 DFL 解码 + sigmoid
//                        （NCNN 官方 yolov8n / PNNX 默认转换的 yolov8 即此格式）
//   2. DECODED_Nx4plusC — 输出 [N, 4 + num_classes]，bbox 已是 xyxy，cls 已 sigmoid
//                        （部分 TF/ONNX 转换的模型把 DFL+decode 写进了网络）
//
// 推理后端: CPU（ARM Neon）或 GPU（Vulkan），可在运行时切换。
#pragma once

#include <vector>
#include <string>
#include <cstdint>

// 前向声明，避免在头文件暴露 NCNN 类型
namespace ncnn { class Net; class Mat; }

namespace aimbot {

struct Detection {
    float x1, y1, x2, y2;  // 像素坐标（原图尺度）
    float confidence;
    int   class_id;
};

// 模型输出格式
enum class OutputFormat {
    DFL_RAW = 0,           // [N, 64 + num_classes]，需 DFL 解码
    DECODED_Nx4plusC = 1,  // [N, 4 + num_classes]，已解码
};

// 模型元数据：描述一个 NCNN 模型的加载与解码参数。
// 由 Kotlin 层根据 assets/models/*.json 读取后传入 nativeInit / nativeLoadModel。
struct ModelMeta {
    std::string name;          // 模型显示名（如 "yolov8n-coco80"）
    int         input_size   = 640;
    std::string input_blob   = "in0";
    std::string output_blob  = "out0";
    int         num_classes  = 80;
    OutputFormat format      = OutputFormat::DFL_RAW;
    // 仅当 format == DECODED_Nx4plusC 且 bbox 不是 xyxy 而是 xywh 时为 true
    bool        bbox_xywh    = false;
};

class YoloV8 {
public:
    YoloV8();
    ~YoloV8();

    YoloV8(const YoloV8&) = delete;
    YoloV8& operator=(const YoloV8&) = delete;

    // 加载模型（带元数据）。
    //   param_path / bin_path: 模型文件路径（assets 拷贝到内部存储后传入）
    //   meta: 模型元数据（输入尺寸/blob 名/类别数/输出格式）
    //   use_gpu: true=Vulkan, false=CPU(ARM Neon)
    //   num_threads: CPU 线程数
    bool load(const std::string& param_path, const std::string& bin_path,
              const ModelMeta& meta,
              bool use_gpu, int num_threads = 4);

    // 释放模型资源。
    void unload();

    // 对一帧 RGBA 数据进行检测。
    //   rgba: RGBA 像素数据（行优先）
    //   img_w / img_h: 原图宽高
    //   conf_thresh: 置信度阈值
    //   nms_thresh: NMS 阈值
    // 返回检测框列表。
    std::vector<Detection> detect(const uint8_t* rgba, int img_w, int img_h,
                                  float conf_thresh = 0.45f,
                                  float nms_thresh = 0.45f);

    // 最近一次推理耗时（毫秒）
    float lastInferenceMs() const;

    bool isLoaded() const;

    const ModelMeta& meta() const { return meta_; }

private:
    // DFL 解码（format == DFL_RAW 时调用）
    //   data: 指向单个 anchor 的 64 + num_classes 维数据
    //   out_ltrb: 输出 [left, top, right, bottom]（在 input_size 尺度下，相对 anchor point）
    //   out_cls_scores: 输出 num_classes 维 sigmoid 后的类别分数
    void decodeDflAnchor(const float* data, float* out_ltrb, float* out_cls_scores);

    // 生成 anchor points 表（grid 中心点 + stride）
    //   根据 input_size 和 strides [8,16,32] 生成
    void buildAnchorPoints();

    ncnn::Net* net_;
    float last_ms_;
    ModelMeta meta_;

    // anchor points: 每行 [cx, cy, stride]
    std::vector<std::tuple<float, float, float>> anchors_;
    int total_anchors_;
};

} // namespace aimbot
