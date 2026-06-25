// yolo_v8.h — NCNN 部署 YOLOv8n 目标检测
//
// 推理后端: CPU（ARM Neon）或 GPU（Vulkan），可在运行时切换。
// 延迟目标: < 50ms（arm64-v8a, YOLOv8n, 640x640）。
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

class YoloV8 {
public:
    YoloV8();
    ~YoloV8();

    YoloV8(const YoloV8&) = delete;
    YoloV8& operator=(const YoloV8&) = delete;

    // 加载模型。
    //   param_path / bin_path: 模型文件路径（assets 拷贝到内部存储后传入）
    //   use_gpu: true=Vulkan, false=CPU(ARM Neon)
    //   num_threads: CPU 线程数
    bool load(const std::string& param_path, const std::string& bin_path,
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

private:
    ncnn::Net* net_;
    float last_ms_;
    int input_size_;   // 模型输入尺寸（640）
};

} // namespace aimbot
