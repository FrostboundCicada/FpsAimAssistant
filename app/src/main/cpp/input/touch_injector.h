// touch_injector.h — 触摸注入统一接口（多后端）
//
// 支持的后端（按反检测强度从高到低）:
//   1. KERNEL_DRIVER  — 独立 .ko 驱动（aim_touch / zero / qx / rt）
//                       直接调用 input_handle_event，不创建虚拟设备
//   2. TWT_DRIVER     — TwT 驱动（内核 syscall + ioctl）
//                       支持 touch/gyro/memory，陀螺仪直接修改传感器值
//   3. GYROSCOPE      — 陀螺仪注入，模拟设备转动，无触摸事件
//   4. UINPUT          — 用户态 /dev/uinput（加固伪装版）
//
// 后端选择策略:
//   init() 时按顺序探测各后端，优先使用最难检测的。
#pragma once

#include <cstdint>
#include <string>

namespace aimbot {

enum class TouchState : int { UP = 0, DOWN = 1 };

// 注入后端类型
enum class InjectBackend : int {
    NONE = -1,
    UINPUT = 0,        // 用户态 /dev/uinput（兼容性好）
    KERNEL_DRIVER = 1, // 独立 .ko 驱动（aim_touch / zero / qx / rt）
    GYROSCOPE = 2,     // 陀螺仪注入（无触摸事件）
    TWT_DRIVER = 3,    // TwT 驱动（syscall + ioctl，支持 touch/gyro/mem）
};

// 触摸注入统一接口
class TouchInjector {
public:
    TouchInjector();
    ~TouchInjector();

    TouchInjector(const TouchInjector&) = delete;
    TouchInjector& operator=(const TouchInjector&) = delete;

    // 初始化: 自动探测最佳后端。
    //   screen_width / screen_height: 屏幕分辨率
    //   preferred: 优先使用的后端（NONE 表示自动选择）
    // 返回实际使用的后端，NONE 表示全部失败。
    InjectBackend init(int screen_width, int screen_height,
                       InjectBackend preferred = InjectBackend::NONE);

    void destroy();
    bool isReady() const;
    InjectBackend backend() const { return backend_; }
    std::string backendName() const;
    std::string lastError() const { return last_error_; }

    // ── 触摸事件接口 ──────────────────────────────────
    bool touchDown(int x, int y);
    bool touchMove(int x, int y);
    bool touchUp();
    bool click(int x, int y, int duration_ms = 0);
    bool swipe(int x1, int y1, int x2, int y2, int steps = 20, int step_delay_us = 2000);

    // ── 陀螺仪专用接口 ──────────────────────────────
    // 注入陀螺仪旋转（仅 GYROSCOPE / TWT_DRIVER 后端有效）
    //   rx, ry, rz: 三轴角速度（毫弧度/秒）
    bool gyroInject(float rx, float ry, float rz);
    // 陀螺仪归零
    bool gyroReset();

private:
    // ── uinput 后端 ──────────────────────────────────
    bool initUinput(int sw, int sh);
    bool ensureRootAccess();
    bool emit(uint16_t type, uint16_t code, int32_t value);
    bool sync();

    // ── 内核驱动后端 ──────────────────────────────────
    bool initKernelDriver(int sw, int sh);
    // 探测已知驱动设备节点
    int openKnownDriver();

    // ── 陀螺仪后端 ──────────────────────────────────
    bool initGyroscope(int sw, int sh);
    // 查找陀螺仪 input 设备
    int findGyroDevice();

    // ── TwT 驱动后端 ──────────────────────────────────
    bool initTwtDriver(int sw, int sh);
    // 通过 /proc/self/fd 查找 TwT_driver 的 anon_inode fd
    int findTwtFd();
    // 通过 syscall(__NR_reboot) 获取 TwT fd（内核 hook）
    int syscallTwtFd();
    // TwT 陀螺仪初始化
    bool twtGyroInit(int method);
    // TwT 触摸初始化
    bool twtTouchInit(int mode);

    // ── 反检测辅助 ──────────────────────────────────
    int randomTrackingId();
    int randomPressure();
    int randomTouchMajor();
    int randomJitterUs(int base_us);
    void applyMicroJitter(int& x, int& y, int amplitude = 1);

    // 内部状态
    int fd_;            // uinput 或驱动设备 fd
    int gyro_fd_;       // 陀螺仪设备 fd
    int screen_w_;
    int screen_h_;
    int tracking_id_;
    TouchState state_;
    InjectBackend backend_;
    mutable std::string last_error_;

    // 陀螺仪参数
    int gyro_abs_x_, gyro_abs_y_, gyro_abs_z_; // ABS_RX/RY/RZ code
    int gyro_max_;      // 陀螺仪量程
};

} // namespace aimbot
