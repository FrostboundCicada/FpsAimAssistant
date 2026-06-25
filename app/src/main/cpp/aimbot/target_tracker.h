// target_tracker.h — 目标跟踪与速度预测
//
// 跨帧关联检测框，维护目标 ID，并基于位置差分估算速度，
// 用于"提前量"瞄准（lead prediction），补偿推理+注入的管线延迟。
#pragma once

#include <vector>
#include "../ncnn/yolo_v8.h"

namespace aimbot {

struct TrackedTarget {
    int   id;             // 跟踪 ID
    float cx, cy;         // 当前中心（像素）
    float vx, vy;         // 速度（像素/帧）
    float w, h;           // 框宽高
    int   class_id;
    float confidence;
    int   lost_frames;    // 丢失帧计数
};

class TargetTracker {
public:
    TargetTracker();

    // 更新跟踪器，输入当前帧检测框。
    //   max_dist: 关联最大距离（像素）
    //   max_lost: 丢失多少帧后删除
    void update(const std::vector<Detection>& dets, float max_dist = 80.f, int max_lost = 5);

    // 选择最佳目标: 距离瞄准点最近且置信度达标。
    //   aim_x, aim_y: 屏幕瞄准点（通常是屏幕中心或准星位置）
    //   max_radius: 最大选取半径
    // 返回 nullptr 表示无合适目标。
    const TrackedTarget* bestTarget(float aim_x, float aim_y, float max_radius) const;

    const std::vector<TrackedTarget>& targets() const { return targets_; }

private:
    std::vector<TrackedTarget> targets_;
    int next_id_;
};

} // namespace aimbot
