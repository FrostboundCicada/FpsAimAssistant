// aimbot_controller.cpp — 瞄准控制器实现（人性化轨迹加固版）
//
// 反检测加固:
//   1. 贝塞尔曲线轨迹: 替代直线插值，模拟人类拖拽的弧形路径
//   2. 速度曲线: 使用 ease-in-out（S 型）而非线性，模拟手指加减速
//   3. 超调与回弹: 模拟人类瞄准时的轻微过冲和修正
//   4. 随机步数和间隔: 每次注入的步数/时序不同，避免指纹识别
//   5. 偶发停顿: 模拟人类犹豫/反应延迟
#include "aimbot_controller.h"

#include <cmath>
#include <cstdlib>
#include <ctime>
#include <unistd.h>          // getpid, usleep
#include <android/log.h>

#define LOG_TAG "aimbot_controller"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace aimbot {

static void ensureSeeded() {
    static bool s = false;
    if (!s) { srand((unsigned)(time(nullptr) ^ getpid())); s = true; }
}

static float bezier3(float p0, float p1, float p2, float p3, float t) {
    float u = 1.f - t;
    return u*u*u*p0 + 3.f*u*u*t*p1 + 3.f*u*t*t*p2 + t*t*t*p3;
}

static float easeInOut(float t) {
    return t < 0.5f ? 2.f * t * t : 1.f - powf(-2.f * t + 2.f, 2.f) / 2.f;
}

AimbotController::AimbotController() : injecting_(false), last_offset_distance_(-1.f), target_switching_(false) {}
AimbotController::~AimbotController() { shutdown(); }

InjectBackend AimbotController::initInjector(int screen_w, int screen_h, InjectBackend preferred) {
    return injector_.init(screen_w, screen_h, preferred);
}

void AimbotController::shutdown() {
    injector_.destroy();
}

// ── 动态速度计算（参考 RookieAI）──────────────────────
// 近距离降速避免过冲，中距离加速，远距离保持基础速度
float AimbotController::computeDynamicSpeed(float offset_distance) {
    float base = cfg_.aim_speed;
    float max_speed = base * cfg_.near_speed_multiplier;

    if (offset_distance < cfg_.slow_zone_radius) {
        // 减速区: 距离越小速度越低，线性插值
        return base + (max_speed - base) * (offset_distance / cfg_.slow_zone_radius);
    } else if (offset_distance < cfg_.aim_radius) {
        // 正常区: 距离越近速度越低（1 - dist/radius）
        return base + (max_speed - base) * (1.f - offset_distance / cfg_.aim_radius);
    }
    // 超出瞄准范围，保持基础速度
    return base;
}

// ── 跳变检测（参考 RookieAI）──────────────────────────
// 检测偏移距离是否突然增大（目标切换），防止准星瞬移被反作弊检测
bool AimbotController::checkJumpDetection(float offset_distance) {
    if (!cfg_.jump_detection_enabled) return false;
    if (last_offset_distance_ >= 0.f && !target_switching_) {
        // 偏移距离突然增大超过波动范围，判定为目标切换
        if (offset_distance > last_offset_distance_ + cfg_.jump_fluctuation_range) {
            target_switching_ = true;
            LOGI("跳变检测: 目标切换，暂停注入");
        }
    }
    last_offset_distance_ = offset_distance;
    return target_switching_;
}

void AimbotController::computeAimPoint(const TrackedTarget& t, float /*aim_x*/, float /*aim_y*/,
                                      float& out_x, float& out_y) {
    if (cfg_.headshot_mode) {
        out_x = t.cx;
        out_y = t.cy - t.h * 0.25f;
    } else {
        out_x = t.cx;
        out_y = t.cy;
    }
    float lead_frames = (cfg_.pipeline_ms / 16.6f) * cfg_.lead_factor;
    out_x += t.vx * lead_frames;
    out_y += t.vy * lead_frames;
}

const TrackedTarget* AimbotController::processFrame(const std::vector<Detection>& dets,
                                                    float aim_x, float aim_y, float /*frame_dt_ms*/) {
    tracker_.update(dets);
    return tracker_.bestTarget(aim_x, aim_y, cfg_.aim_radius);
}

// ── 触摸注入: 贝塞尔人性化轨迹 ──────────────────────

void AimbotController::aimOnce(float aim_x, float aim_y) {
    if (!cfg_.trigger_enabled) return;
    if (!injector_.isReady()) return;
    // 陀螺仪后端不使用触摸注入
    if (injector_.backend() == InjectBackend::GYROSCOPE) return;
    if (injecting_.exchange(true)) return;

    const TrackedTarget* t = tracker_.bestTarget(aim_x, aim_y, cfg_.aim_radius);
    if (!t) {
        // 无目标时重置跳变检测状态
        last_offset_distance_ = -1.f;
        target_switching_ = false;
        injecting_ = false;
        return;
    }

    float tx, ty;
    computeAimPoint(*t, aim_x, aim_y, tx, ty);

    float dx = tx - aim_x;
    float dy = ty - aim_y;
    float dist = sqrtf(dx * dx + dy * dy);

    // 跳变检测: 目标切换时拒绝注入（参考 RookieAI）
    if (checkJumpDetection(dist)) {
        injecting_ = false;
        return;
    }

    ensureSeeded();

    // 触摸后端灵敏度: X/Y 分别缩放目标偏移，使 X/Y 滑块独立生效
    // sens>1 -> 偏移放大 -> 单次注入移动更远 -> 更快接近目标
    // sens<1 -> 偏移缩小 -> 移动更平缓
    float eff_dx = dx * cfg_.sens_x;
    float eff_dy = dy * cfg_.sens_y;
    float eff_dist = sqrtf(eff_dx * eff_dx + eff_dy * eff_dy);
    // 限制单次注入最大位移，避免灵敏度过高导致瞬移被检测
    float max_step = dist;  // 不超过真实目标距离
    if (eff_dist > max_step && eff_dist > 0.001f) {
        float scale = max_step / eff_dist;
        eff_dx *= scale;
        eff_dy *= scale;
        eff_dist = max_step;
    }
    // 实际注入目标点（朝目标方向前进 eff_dist，未到则下次继续）
    float inject_tx = aim_x + eff_dx;
    float inject_ty = aim_y + eff_dy;

    float perp_x = -eff_dy / (eff_dist + 0.001f);
    float perp_y =  eff_dx / (eff_dist + 0.001f);
    float arc_sign = (rand() % 2 == 0) ? 1.f : -1.f;
    float arc_amp = eff_dist * (0.1f + (rand() % 20) / 100.f);
    float cx1 = aim_x + eff_dx * 0.33f + perp_x * arc_amp * arc_sign;
    float cy1 = aim_y + eff_dy * 0.33f + perp_y * arc_amp * arc_sign;
    float cx2 = aim_x + eff_dx * 0.66f + perp_x * arc_amp * arc_sign * 0.5f;
    float cy2 = aim_y + eff_dy * 0.66f + perp_y * arc_amp * arc_sign * 0.5f;

    // 步数: aim_speed 越高 -> 步数越少 -> 移动越快（修复触摸后端速度滑块不生效）
    int steps = 8 + (int)(eff_dist / 50.f);
    float speed_factor = computeDynamicSpeed(eff_dist);  // 复用动态速度（含 aim_speed）
    if (speed_factor > 0.01f) steps = (int)(steps / speed_factor);
    if (steps > 20) steps = 20;
    if (steps < 6) steps = 6;

    bool overshoot = (rand() % 100) < 30;
    float overshoot_x = 0.f, overshoot_y = 0.f;
    if (overshoot) {
        float os = 2.f + (rand() % 6);
        overshoot_x = (eff_dx / (eff_dist + 0.001f)) * os;
        overshoot_y = (eff_dy / (eff_dist + 0.001f)) * os;
    }

    float sx = aim_x, sy = aim_y;
    injector_.touchDown((int)sx, (int)sy);
    usleep(2000 + (rand() % 3000));

    for (int i = 1; i <= steps; ++i) {
        float t_norm = (float)i / steps;
        float eased = easeInOut(t_norm);
        sx = bezier3(aim_x, cx1, cx2, inject_tx, eased);
        sy = bezier3(aim_y, cy1, cy2, inject_ty, eased);
        if (overshoot && i == steps) {
            sx += overshoot_x;
            sy += overshoot_y;
        }
        injector_.touchMove((int)sx, (int)sy);
        int base_us = (int)(800 + 600 * fabsf(0.5f - eased) * 2.f);
        int jitter = base_us * 30 / 100;
        usleep(base_us - jitter + (rand() % (jitter * 2 + 1)));
    }

    if (overshoot) {
        usleep(1000 + (rand() % 2000));
        for (int i = 1; i <= 3; ++i) {
            float t = (float)i / 3.f;
            float bx = (inject_tx + overshoot_x) * (1.f - t) + inject_tx * t;
            float by = (inject_ty + overshoot_y) * (1.f - t) + inject_ty * t;
            injector_.touchMove((int)bx, (int)by);
            usleep(600 + (rand() % 800));
        }
    }

    if (rand() % 100 < 20) usleep(1500 + (rand() % 2000));
    injector_.touchUp();
    injecting_ = false;
    LOGI("触摸注入: (%.0f,%.0f)->(%.0f,%.0f) dist=%.0f eff=%.0f steps=%d sens=%.2f/%.2f",
         aim_x, aim_y, inject_tx, inject_ty, dist, eff_dist, steps, cfg_.sens_x, cfg_.sens_y);
}

// ── 陀螺仪注入: 持续微调 ──────────────────────────────
// 陀螺仪瞄准不使用触摸事件，而是注入角速度让游戏以为设备在转动
// 优势: 完全无触摸特征，反作弊无法通过触摸行为检测
//
// 原理: 像素偏移 -> 角速度（dps）-> 陀螺仪 ABS 值
// 游戏的陀螺仪瞄准代码会根据角速度移动准星

void AimbotController::gyroAim(float dx, float dy) {
    if (!cfg_.trigger_enabled) return;
    if (!injector_.isReady()) return;
    // 陀螺仪后端或 TwT 驱动都支持陀螺仪注入
    if (injector_.backend() != InjectBackend::GYROSCOPE &&
        injector_.backend() != InjectBackend::TWT_DRIVER) return;

    float offset_distance = sqrtf(dx * dx + dy * dy);

    // 跳变检测: 目标切换时暂停注入，避免准星瞬移被检测
    if (checkJumpDetection(offset_distance)) {
        // 目标切换，归零陀螺仪并等待
        injector_.gyroReset();
        return;
    }

    // 动态速度: 近距离减速，中距离加速（参考 RookieAI）
    float speed_factor = computeDynamicSpeed(offset_distance);

    // 像素偏移 -> 角度偏移（参考 RookieAI 的 pixels_per_degree）
    float angle_offset_x = dx / cfg_.pixels_per_degree_x;  // 度
    float angle_offset_y = dy / cfg_.pixels_per_degree_y;  // 度

    // 应用动态速度 + X/Y 轴灵敏度倍率
    float target_ry = angle_offset_x * speed_factor * 2.f * cfg_.gyro_sensitivity * cfg_.sens_x;
    float target_rx = -angle_offset_y * speed_factor * 2.f * cfg_.gyro_sensitivity * cfg_.sens_y;
    float target_rz = 0.f;

    // 限制最大角速度
    float max_dps = 500.f;
    if (target_rx > max_dps) target_rx = max_dps;
    if (target_rx < -max_dps) target_rx = -max_dps;
    if (target_ry > max_dps) target_ry = max_dps;
    if (target_ry < -max_dps) target_ry = -max_dps;

    // 微小随机抖动（模拟手部不稳）
    ensureSeeded();
    target_rx += (rand() % 100 - 50) * 0.02f;
    target_ry += (rand() % 100 - 50) * 0.02f;

    // 注入（dps -> 毫弧度/秒: 1 dps ≈ 17.453 mrad/s）
    injector_.gyroInject(target_rx * 17.453f, target_ry * 17.453f, target_rz * 17.453f);
}

} // namespace aimbot
