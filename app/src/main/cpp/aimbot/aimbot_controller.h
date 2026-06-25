// aimbot_controller.h — 瞄准控制器（多后端注入版）
//
// 职责: 串联 检测 -> 跟踪 -> 目标选择 -> 提前量计算 -> 注入。
// 支持触摸注入和陀螺仪注入两种模式。
#pragma once

#include <vector>
#include <atomic>
#include <memory>
#include "../ncnn/yolo_v8.h"
#include "../input/touch_injector.h"
#include "target_tracker.h"

namespace aimbot {

struct AimConfig {
    float aim_radius      = 400.f;  // 目标选取半径（像素）
    float aim_speed       = 0.35f;  // 基础平滑系数（0~1）
    float lead_factor     = 1.0f;   // 提前量系数
    float pipeline_ms     = 50.f;   // 管线延迟估计（ms）
    float fov_scale       = 1.0f;   // 视野缩放
    bool  headshot_mode   = false;  // true=瞄头部
    bool  trigger_enabled = false;  // 是否启用自动注入
    // 陀螺仪参数
    float gyro_sensitivity = 1.0f;

    // ── 参考 RookieAI 的动态速度算法 ──────────────────
    float near_speed_multiplier = 2.0f;  // 近距离速度倍率（最大速度 = base * multiplier）
    float slow_zone_radius      = 50.f;  // 减速区半径（像素），进入此范围降速避免过冲
    // 像素→角度转换（用于陀螺仪瞄准）
    // 典型 FPS: 屏幕宽 1080 对应约 90 度视野 → pixels_per_degree ≈ 12
    float pixels_per_degree_x   = 12.0f;
    float pixels_per_degree_y   = 12.0f;

    // ── 跳变检测（防目标切换瞬移被检测）──────────────────
    bool  jump_detection_enabled = true;   // 跳变检测开关
    float jump_fluctuation_range = 30.f;  // 允许的偏移波动范围（像素），超过视为目标切换
    // 自动扳机范围（基于检测框宽度）
    float auto_trigger_scale    = 0.5f;   // 自动扳机半径 = 框宽 * scale / 2
};

class AimbotController {
public:
    AimbotController();
    ~AimbotController();

    // 初始化注入设备（屏幕分辨率）
    //   preferred: 优先后端（NONE 表示自动探测）
    InjectBackend initInjector(int screen_w, int screen_h,
                                InjectBackend preferred = InjectBackend::NONE);

    void shutdown();

    const TrackedTarget* processFrame(const std::vector<Detection>& dets,
                                      float aim_x, float aim_y, float frame_dt_ms);

    // 触摸注入一次瞄准（按下->贝塞尔移动->抬起）
    void aimOnce(float aim_x, float aim_y);

    // 陀螺仪注入: 持续微调准星（每帧调用）
    //   dx, dy: 目标相对准星的偏移（像素）
    void gyroAim(float dx, float dy);

    AimConfig& config() { return cfg_; }
    const AimConfig& config() const { return cfg_; }

    TouchInjector& injector() { return injector_; }
    TargetTracker& tracker() { return tracker_; }

private:
    void computeAimPoint(const TrackedTarget& t, float aim_x, float aim_y,
                         float& out_x, float& out_y);

    // 动态速度计算（参考 RookieAI）
    // 返回当前偏移距离对应的 aim_speed
    float computeDynamicSpeed(float offset_distance);

    // 跳变检测: 判断是否发生目标切换
    bool checkJumpDetection(float offset_distance);

    TouchInjector injector_;
    TargetTracker tracker_;
    AimConfig cfg_;
    std::atomic<bool> injecting_;

    // 跳变检测状态
    float last_offset_distance_;
    bool  target_switching_;
};

} // namespace aimbot
