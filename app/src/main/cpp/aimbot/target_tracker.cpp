// target_tracker.cpp — 目标跟踪实现
#include "target_tracker.h"

#include <cmath>
#include <algorithm>

namespace aimbot {

TargetTracker::TargetTracker() : next_id_(1) {}

static float dist2(float ax, float ay, float bx, float by) {
    float dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy;
}

void TargetTracker::update(const std::vector<Detection>& dets, float max_dist, int max_lost) {
    std::vector<int> matched(dets.size(), 0);

    // 贪心关联: 每个已跟踪目标找最近的未匹配检测
    for (auto& t : targets_) {
        float best_d = max_dist * max_dist;
        int best_j = -1;
        for (size_t j = 0; j < dets.size(); ++j) {
            if (matched[j]) continue;
            float cx = (dets[j].x1 + dets[j].x2) * 0.5f;
            float cy = (dets[j].y1 + dets[j].y2) * 0.5f;
            float d = dist2(cx, cy, t.cx, t.cy);
            if (d < best_d) { best_d = d; best_j = (int)j; }
        }
        if (best_j >= 0) {
            const Detection& d = dets[best_j];
            float ncx = (d.x1 + d.x2) * 0.5f;
            float ncy = (d.y1 + d.y2) * 0.5f;
            // 速度 = 新位置 - 旧位置（指数平滑，避免抖动）
            float new_vx = ncx - t.cx;
            float new_vy = ncy - t.cy;
            t.vx = t.vx * 0.6f + new_vx * 0.4f;
            t.vy = t.vy * 0.6f + new_vy * 0.4f;
            t.cx = ncx; t.cy = ncy;
            t.w = d.x2 - d.x1; t.h = d.y2 - d.y1;
            t.class_id = d.class_id;
            t.confidence = d.confidence;
            t.lost_frames = 0;
            matched[best_j] = 1;
        } else {
            t.lost_frames++;
        }
    }

    // 删除丢失过久的目标
    targets_.erase(
        std::remove_if(targets_.begin(), targets_.end(),
                       [&](const TrackedTarget& t) { return t.lost_frames > max_lost; }),
        targets_.end());

    // 未匹配的检测作为新目标
    for (size_t j = 0; j < dets.size(); ++j) {
        if (matched[j]) continue;
        const Detection& d = dets[j];
        TrackedTarget t;
        t.id = next_id_++;
        t.cx = (d.x1 + d.x2) * 0.5f;
        t.cy = (d.y1 + d.y2) * 0.5f;
        t.vx = 0; t.vy = 0;
        t.w = d.x2 - d.x1; t.h = d.y2 - d.y1;
        t.class_id = d.class_id;
        t.confidence = d.confidence;
        t.lost_frames = 0;
        targets_.push_back(t);
    }
}

const TrackedTarget* TargetTracker::bestTarget(float aim_x, float aim_y, float max_radius) const {
    const TrackedTarget* best = nullptr;
    float best_d = max_radius * max_radius;
    for (const auto& t : targets_) {
        if (t.lost_frames > 0) continue;
        float d = dist2(t.cx, t.cy, aim_x, aim_y);
        if (d < best_d) { best_d = d; best = &t; }
    }
    return best;
}

} // namespace aimbot
