// touch_injector.cpp — 多后端触摸注入实现
//
// 后端探测顺序:
//   1. 内核驱动（aim_touch / zero / qx / rt）— 最难检测
//   2. 陀螺仪 — 无触摸事件，适合支持陀螺仪瞄准的游戏
//   3. uinput — 兼容性最好，加固伪装
#include "touch_injector.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <stdlib.h>
#include <sys/syscall.h>   // __NR_reboot
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/input.h>
#include <linux/uinput.h>

#include <android/log.h>

#define LOG_TAG "touch_injector"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifndef UINPUT_MAX_NAME_SIZE
#define UINPUT_MAX_NAME_SIZE 80
#endif

// ── 内核驱动 ioctl 命令 ──────────────────────────────
// 与 kernel/touch_driver.c 一致
#define AIM_TOUCH_MAGIC 'A'
struct aim_touch_point { int x, y, pressure, touch_major, tracking_id; };
struct aim_touch_init { int screen_w, screen_h; };
#define AIM_TOUCH_DOWN _IOW(AIM_TOUCH_MAGIC, 1, struct aim_touch_point)
#define AIM_TOUCH_MOVE _IOW(AIM_TOUCH_MAGIC, 2, struct aim_touch_point)
#define AIM_TOUCH_UP   _IO(AIM_TOUCH_MAGIC, 3)
#define AIM_TOUCH_INIT _IOW(AIM_TOUCH_MAGIC, 4, struct aim_touch_init)

namespace aimbot {

static void ensureRngSeeded() {
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned)(time(nullptr) ^ getpid()));
        seeded = true;
    }
}

TouchInjector::TouchInjector()
    : fd_(-1), gyro_fd_(-1), screen_w_(0), screen_h_(0), tracking_id_(0),
      state_(TouchState::UP), backend_(InjectBackend::NONE),
      gyro_abs_x_(0), gyro_abs_y_(0), gyro_abs_z_(0), gyro_max_(0) {}

TouchInjector::~TouchInjector() { destroy(); }

bool TouchInjector::isReady() const {
    if (backend_ == InjectBackend::GYROSCOPE) return gyro_fd_ >= 0;
    if (backend_ == InjectBackend::TWT_DRIVER) return fd_ >= 0;
    return fd_ >= 0;
}

std::string TouchInjector::backendName() const {
    switch (backend_) {
    case InjectBackend::UINPUT:       return "uinput (加固伪装)";
    case InjectBackend::KERNEL_DRIVER: return "内核驱动 (input_handle_event)";
    case InjectBackend::GYROSCOPE:   return "陀螺仪注入";
    case InjectBackend::TWT_DRIVER:  return "TwT 驱动 (syscall+ioctl)";
    default: return "无";
    }
}

// ── 反检测辅助 ──────────────────────────────────────

int TouchInjector::randomTrackingId() {
    ensureRngSeeded();
    return (rand() % 60000) + 1000;
}
int TouchInjector::randomPressure() {
    ensureRngSeeded();
    return 80 + (rand() % 80);
}
int TouchInjector::randomTouchMajor() {
    ensureRngSeeded();
    return 8 + (rand() % 9);
}
int TouchInjector::randomJitterUs(int base_us) {
    ensureRngSeeded();
    int jitter = base_us * 30 / 100;
    if (jitter == 0) jitter = 1;
    return base_us - jitter + (rand() % (jitter * 2 + 1));
}
void TouchInjector::applyMicroJitter(int& x, int& y, int amplitude) {
    ensureRngSeeded();
    x += (rand() % (amplitude * 2 + 1)) - amplitude;
    y += (rand() % (amplitude * 2 + 1)) - amplitude;
}

// ── 初始化: 自动探测最佳后端 ──────────────────────────

InjectBackend TouchInjector::init(int screen_width, int screen_height,
                                   InjectBackend preferred) {
    screen_w_ = screen_width;
    screen_h_ = screen_height;

    if (fd_ >= 0 || gyro_fd_ >= 0) destroy();

    // 若指定了优先后端，先尝试它
    if (preferred != InjectBackend::NONE) {
        bool ok = false;
        switch (preferred) {
        case InjectBackend::KERNEL_DRIVER: ok = initKernelDriver(screen_width, screen_height); break;
        case InjectBackend::TWT_DRIVER:    ok = initTwtDriver(screen_width, screen_height); break;
        case InjectBackend::GYROSCOPE:     ok = initGyroscope(screen_width, screen_height); break;
        case InjectBackend::UINPUT:       ok = initUinput(screen_width, screen_height); break;
        default: break;
        }
        if (ok) {
            LOGI("使用指定后端: %s", backendName().c_str());
            return backend_;
        }
        LOGW("指定后端 %d 失败，尝试自动探测", (int)preferred);
    }

    // 自动探测顺序: TwT -> 内核驱动 -> 陀螺仪 -> uinput
    // TwT 最优先（内核 syscall + 直接改陀螺仪值，最难检测）
    if (initTwtDriver(screen_width, screen_height)) {
        LOGI("自动选择: TwT 驱动");
        return backend_;
    }
    if (initKernelDriver(screen_width, screen_height)) {
        LOGI("自动选择: 内核驱动");
        return backend_;
    }
    if (initGyroscope(screen_width, screen_height)) {
        LOGI("自动选择: 陀螺仪注入");
        return backend_;
    }
    if (initUinput(screen_width, screen_height)) {
        LOGI("自动选择: uinput (加固)");
        return backend_;
    }

    backend_ = InjectBackend::NONE;
    last_error_ = "所有注入后端均不可用";
    LOGE("%s", last_error_.c_str());
    return InjectBackend::NONE;
}

// ── Root 权限 ────────────────────────────────────────

bool TouchInjector::ensureRootAccess() {
    if (access("/dev/uinput", W_OK) == 0) return true;
    int rc = system("su -c 'chmod 666 /dev/uinput' 2>/dev/null");
    if (rc != 0) rc = system("su 0 chmod 666 /dev/uinput 2>/dev/null");
    if (rc != 0 || access("/dev/uinput", W_OK) != 0) {
        last_error_ = "无法获取 /dev/uinput 写权限，请确认 Root";
        LOGE("%s", last_error_.c_str());
        return false;
    }
    return true;
}

// ── 内核驱动后端 ──────────────────────────────────────
// 探测已知驱动设备节点: aim_touch / zero / qx / rt

int TouchInjector::openKnownDriver() {
    // 已知驱动设备节点列表（按优先级）
    const char* drivers[] = {
        "/dev/aim_touch",    // 本工程自带驱动
        "/dev/zero_touch",   // ZeroDriver
        "/dev/zero",         // ZeroDriver 变体
        "/dev/qx_touch",     // QX 驱动
        "/dev/qx",           // QX 变体
        "/dev/rt_touch",     // rt 驱动
        "/dev/rt",           // rt 变体
        "/dev/ovo_touch",    // ovo 驱动
        "/dev/hakutaku",     // hakutaku 驱动
    };
    for (const char* path : drivers) {
        int fd = open(path, O_WRONLY);
        if (fd >= 0) {
            LOGI("检测到内核驱动: %s", path);
            return fd;
        }
    }
    return -1;
}

// ── TwT 驱动后端 ──────────────────────────────────────
// TwT 通过两种方式获取 fd:
//   1. syscall(__NR_reboot) — 内核 hook reboot 系统调用，返回驱动 fd
//   2. /proc/self/fd 查找 anon_inode 中含 "TwT_driver" 的 fd
//
// TwT 接口（参考对接 v1.45）:
//   TOUCH_INIT(slot_mode)  — 初始化触摸（mode 0/1）
//   TOUCH_DOWN(slot,x,y)  — 触摸按下
//   TOUCH_UP(slot)        — 触摸抬起
//   GYRO_INIT(method)     — 初始化陀螺仪（method 0/1，0 已被特征）
//   GYRO_CONFIG(enable,x,y) — 修改陀螺仪值（直接改传感器，非注入事件）

#define TWT_MARK 'T'
struct twt_touch_event { int slot; int x; int y; };
struct twt_gyro_config { uint32_t enable; uint32_t x; uint32_t y; };
#define TWT_TOUCH_INIT _IOW(TWT_MARK, 6, struct twt_touch_event)
#define TWT_TOUCH_DOWN _IOW(TWT_MARK, 7, struct twt_touch_event)
#define TWT_TOUCH_UP   _IOW(TWT_MARK, 8, struct twt_touch_event)
#define TWT_GYRO_INIT  _IOW(TWT_MARK, 9, int)
#define TWT_GYRO_CONFIG _IOWR(TWT_MARK, 10, struct twt_gyro_config)

// 通过 /proc/self/fd 查找 TwT_driver 的 anon_inode fd
int TouchInjector::findTwtFd() {
    DIR* dir = opendir("/proc/self/fd");
    if (!dir) return -1;

    int found_fd = -1;
    struct dirent* entry;
    char path[64], link[256];
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        snprintf(path, sizeof(path), "/proc/self/fd/%s", entry->d_name);
        ssize_t len = readlink(path, link, sizeof(link) - 1);
        if (len < 0) continue;
        link[len] = '\0';
        // TwT 的 fd 是 anon_inode 且链接名含 "TwT_driver"
        if (strstr(link, "TwT_driver") != nullptr && strstr(link, "anon_inode:") != nullptr) {
            found_fd = atoi(entry->d_name);
            break;
        }
    }
    closedir(dir);
    return found_fd;
}

// 通过 syscall(__NR_reboot) 获取 TwT fd
// TwT 内核模块 hook 了 reboot 系统调用，特定 magic 值会返回驱动 fd
int TouchInjector::syscallTwtFd() {
#ifdef __aarch64__
    // TwT 的 magic: 0x114514, 0x1919810, cmd 0x2778
    // 使用内联汇编直接 syscall（参考 kernel.h 的 MY_CALL 宏）
    long ret;
    register long _x0 __asm__("x0") = 0x114514;
    register long _x1 __asm__("x1") = 0x1919810;
    register long _x2 __asm__("x2") = 0x2778;
    register long _x3 __asm__("x3") = (long)&ret;
    register long _x8 __asm__("x8") = __NR_reboot;  // syscall number
    __asm__ __volatile__(
        "svc #0"
        : "=r"(_x0)
        : "r"(_x0), "r"(_x1), "r"(_x2), "r"(_x3), "r"(_x8)
        : "memory", "cc"
    );
    // 返回值在 _x0，但 TwT 把 fd 写入 _x3 指向的地址
    return (int)ret;
#else
    return -1;
#endif
}

bool TouchInjector::twtTouchInit(int mode) {
    if (fd_ < 0) return false;
    struct twt_touch_event teb = {};
    teb.slot = mode;  // 0=方案一, 1=方案二
    if (ioctl(fd_, TWT_TOUCH_INIT, &teb) != 0) {
        if (errno == EALREADY) {
            LOGI("TwT 触摸已开启，模式: %d", teb.slot);
            return true;
        }
        last_error_ = std::string("TwT TOUCH_INIT 失败: ") + strerror(errno);
        return false;
    }
    return true;
}

bool TouchInjector::twtGyroInit(int method) {
    if (fd_ < 0) return false;
    int m = method;
    if (ioctl(fd_, TWT_GYRO_INIT, &m) != 0) {
        if (errno == EALREADY) {
            LOGI("TwT 陀螺仪已开启，模式: %d", m);
            return true;
        }
        last_error_ = std::string("TwT GYRO_INIT 失败: ") + strerror(errno);
        return false;
    }
    return true;
}

bool TouchInjector::initTwtDriver(int /*sw*/, int /*sh*/) {
    // 方式1: 通过 syscall 获取 fd（TwT 内核 hook reboot）
    fd_ = syscallTwtFd();
    if (fd_ < 0) {
        // 方式2: 通过 /proc/self/fd 查找
        fd_ = findTwtFd();
    }
    if (fd_ < 0) {
        last_error_ = "未检测到 TwT 驱动";
        return false;
    }

    LOGI("TwT 驱动 fd 获取成功: %d", fd_);

    // 初始化触摸（方案二，兼容性更好；方案一已被部分游戏特征）
    if (!twtTouchInit(1)) {
        LOGW("TwT 触摸方案二失败，尝试方案一");
        if (!twtTouchInit(0)) {
            LOGW("TwT 触摸初始化失败，仅使用陀螺仪");
        }
    }

    // 初始化陀螺仪（方案二，方案一已被特征）
    if (!twtGyroInit(1)) {
        LOGW("TwT 陀螺仪方案二失败");
    }

    backend_ = InjectBackend::TWT_DRIVER;
    state_ = TouchState::UP;
    return true;
}

bool TouchInjector::initKernelDriver(int sw, int sh) {
    fd_ = openKnownDriver();
    if (fd_ < 0) {
        last_error_ = "未检测到内核驱动 (aim_touch/zero/qx/rt)";
        return false;
    }
    // 发送初始化命令（兼容各驱动，失败则忽略——部分驱动无需 init）
    struct aim_touch_init init = { sw, sh };
    ioctl(fd_, AIM_TOUCH_INIT, &init);
    backend_ = InjectBackend::KERNEL_DRIVER;
    state_ = TouchState::UP;
    return true;
}

// ── 陀螺仪后端 ────────────────────────────────────────
// 查找陀螺仪 input 设备并打开写入
// 陀螺仪设备支持 EV_ABS + ABS_RX/ABS_RY/ABS_RZ

int TouchInjector::findGyroDevice() {
    DIR* dir = opendir("/dev/input");
    if (!dir) return -1;

    int found_fd = -1;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;

        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int fd = open(path, O_WRONLY);
        if (fd < 0) continue;

        // 读取设备能力位图
        unsigned long evbit = 0, absbit = 0;
        ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), &evbit);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), &absbit);

        // 陀螺仪: 支持 EV_ABS + ABS_RX/RY/RZ
        bool has_abs = evbit & (1 << EV_ABS);
        bool has_rx = absbit & (1ULL << ABS_RX);
        bool has_ry = absbit & (1ULL << ABS_RY);
        bool has_rz = absbit & (1ULL << ABS_RZ);

        if (has_abs && (has_rx || has_ry || has_rz)) {
            // 进一步确认不是触摸屏（无 ABS_MT_POSITION_X）
            bool has_mt = absbit & (1ULL << ABS_MT_POSITION_X);
            if (!has_mt) {
                LOGI("找到陀螺仪设备: %s (RX=%d RY=%d RZ=%d)",
                     path, has_rx, has_ry, has_rz);
                gyro_abs_x_ = has_rx ? ABS_RX : -1;
                gyro_abs_y_ = has_ry ? ABS_RY : -1;
                gyro_abs_z_ = has_rz ? ABS_RZ : -1;

                // 读取量程
                struct input_absinfo info;
                if (gyro_abs_x_ >= 0 && ioctl(fd, EVIOCGABS(gyro_abs_x_), &info) == 0) {
                    gyro_max_ = info.maximum;
                }
                if (gyro_max_ == 0) gyro_max_ = 32767;

                found_fd = fd;
                break;
            }
        }
        close(fd);
    }
    closedir(dir);
    return found_fd;
}

bool TouchInjector::initGyroscope(int /*sw*/, int /*sh*/) {
    gyro_fd_ = findGyroDevice();
    if (gyro_fd_ < 0) {
        last_error_ = "未找到陀螺仪设备";
        return false;
    }
    backend_ = InjectBackend::GYROSCOPE;
    state_ = TouchState::UP;
    return true;
}

// ── uinput 后端（加固伪装版）────────────────────────

bool TouchInjector::initUinput(int screen_width, int screen_height) {
    if (!ensureRootAccess()) return false;

    fd_ = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd_ < 0) {
        last_error_ = std::string("打开 /dev/uinput 失败: ") + strerror(errno);
        return false;
    }

    if (ioctl(fd_, UI_SET_EVBIT, EV_ABS) < 0 ||
        ioctl(fd_, UI_SET_EVBIT, EV_SYN) < 0 ||
        ioctl(fd_, UI_SET_EVBIT, EV_KEY) < 0) {
        last_error_ = std::string("UI_SET_EVBIT 失败: ") + strerror(errno);
        close(fd_); fd_ = -1;
        return false;
    }

    struct AxisReg { uint16_t code; const char* name; };
    AxisReg axes[] = {
        { ABS_MT_SLOT, "ABS_MT_SLOT" }, { ABS_MT_TRACKING_ID, "ABS_MT_TRACKING_ID" },
        { ABS_MT_POSITION_X, "ABS_MT_POSITION_X" }, { ABS_MT_POSITION_Y, "ABS_MT_POSITION_Y" },
        { ABS_MT_PRESSURE, "ABS_MT_PRESSURE" }, { ABS_MT_TOUCH_MAJOR, "ABS_MT_TOUCH_MAJOR" },
        { ABS_MT_WIDTH_MAJOR, "ABS_MT_WIDTH_MAJOR" },
    };
    for (const auto& a : axes) {
        if (ioctl(fd_, UI_SET_ABSBIT, a.code) < 0) {
            last_error_ = std::string("UI_SET_ABSBIT 失败 (") + a.name + ")";
            close(fd_); fd_ = -1;
            return false;
        }
    }
    ioctl(fd_, UI_SET_KEYBIT, BTN_TOUCH);

    struct uinput_user_dev udev;
    memset(&udev, 0, sizeof(udev));
    // 伪装为真实触摸屏
    const char* fake_names[] = { "fts_ts", "goodix_ts", "synaptics_dsx", "atmel_mxt_ts", "novatek,NVT-ts" };
    ensureRngSeeded();
    snprintf(udev.name, UINPUT_MAX_NAME_SIZE, "%s", fake_names[rand() % 5]);
    udev.id.bustype = BUS_I2C;
    int fake_ids[][3] = {
        {0x2717, 0x1011, 1}, {0x2808, 0x1010, 1}, {0x06cb, 1, 1}, {0x03eb, 0x2115, 1},
    };
    int* id = fake_ids[rand() % 4];
    udev.id.vendor = id[0]; udev.id.product = id[1]; udev.id.version = id[2];

    udev.absmax[ABS_MT_POSITION_X] = screen_width > 0 ? screen_width - 1 : 1080;
    udev.absmax[ABS_MT_POSITION_Y] = screen_height > 0 ? screen_height - 1 : 2400;
    udev.absmax[ABS_MT_TRACKING_ID] = 65535;
    udev.absmax[ABS_MT_SLOT] = 9;
    udev.absmax[ABS_MT_PRESSURE] = 255;
    udev.absmax[ABS_MT_TOUCH_MAJOR] = 255;
    udev.absmax[ABS_MT_WIDTH_MAJOR] = 255;

    if (write(fd_, &udev, sizeof(udev)) < 0 || ioctl(fd_, UI_DEV_CREATE) < 0) {
        last_error_ = std::string("uinput 设备创建失败: ") + strerror(errno);
        close(fd_); fd_ = -1;
        return false;
    }
    usleep(100 * 1000);
    backend_ = InjectBackend::UINPUT;
    state_ = TouchState::UP;
    LOGI("uinput 设备创建成功(加固): %s", udev.name);
    return true;
}

// ── 事件写入 ──────────────────────────────────────────

bool TouchInjector::emit(uint16_t type, uint16_t code, int32_t value) {
    if (fd_ < 0) return false;
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type; ev.code = code; ev.value = value;
    return write(fd_, &ev, sizeof(ev)) == sizeof(ev);
}

bool TouchInjector::sync() { return emit(EV_SYN, SYN_REPORT, 0); }

// ── 触摸事件 ─────────────────────────────────────────

bool TouchInjector::touchDown(int x, int y) {
    if (!isReady()) { last_error_ = "设备未初始化"; return false; }
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (screen_w_ > 0 && x >= screen_w_) x = screen_w_ - 1;
    if (screen_h_ > 0 && y >= screen_h_) y = screen_h_ - 1;
    if (state_ == TouchState::DOWN) return touchMove(x, y);

    tracking_id_ = randomTrackingId();
    applyMicroJitter(x, y, 1);
    int pressure = randomPressure();
    int touch_major = randomTouchMajor();

    if (backend_ == InjectBackend::TWT_DRIVER) {
        // TwT: TOUCH_DOWN(slot, x, y)
        struct twt_touch_event teb = { 0, x, y };
        if (ioctl(fd_, TWT_TOUCH_DOWN, &teb) < 0) {
            last_error_ = std::string("TwT TOUCH_DOWN 失败: ") + strerror(errno);
            return false;
        }
        state_ = TouchState::DOWN;
        return true;
    }

    if (backend_ == InjectBackend::KERNEL_DRIVER) {
        struct aim_touch_point pt = { x, y, pressure, touch_major, tracking_id_ };
        if (ioctl(fd_, AIM_TOUCH_DOWN, &pt) < 0) {
            last_error_ = std::string("驱动 TOUCH_DOWN 失败: ") + strerror(errno);
            return false;
        }
        state_ = TouchState::DOWN;
        return true;
    }

    // uinput
    emit(EV_ABS, ABS_MT_SLOT, 0);
    emit(EV_ABS, ABS_MT_TRACKING_ID, tracking_id_);
    emit(EV_ABS, ABS_MT_POSITION_X, x);
    emit(EV_ABS, ABS_MT_POSITION_Y, y);
    emit(EV_ABS, ABS_MT_PRESSURE, pressure);
    emit(EV_ABS, ABS_MT_TOUCH_MAJOR, touch_major);
    emit(EV_ABS, ABS_MT_WIDTH_MAJOR, touch_major);
    emit(EV_KEY, BTN_TOUCH, 1);
    bool ok = sync();
    if (ok) state_ = TouchState::DOWN;
    return ok;
}

bool TouchInjector::touchMove(int x, int y) {
    if (!isReady()) { last_error_ = "设备未初始化"; return false; }
    if (state_ != TouchState::DOWN) return touchDown(x, y);
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (screen_w_ > 0 && x >= screen_w_) x = screen_w_ - 1;
    if (screen_h_ > 0 && y >= screen_h_) y = screen_h_ - 1;
    applyMicroJitter(x, y, 1);
    int pressure = randomPressure();

    if (backend_ == InjectBackend::TWT_DRIVER) {
        // TwT: 移动用 TOUCH_DOWN 持续报送（TwT 接口设计）
        struct twt_touch_event teb = { 0, x, y };
        if (ioctl(fd_, TWT_TOUCH_DOWN, &teb) < 0) return false;
        return true;
    }

    if (backend_ == InjectBackend::KERNEL_DRIVER) {
        struct aim_touch_point pt = { x, y, pressure, randomTouchMajor(), tracking_id_ };
        if (ioctl(fd_, AIM_TOUCH_MOVE, &pt) < 0) return false;
        return true;
    }

    emit(EV_ABS, ABS_MT_SLOT, 0);
    emit(EV_ABS, ABS_MT_POSITION_X, x);
    emit(EV_ABS, ABS_MT_POSITION_Y, y);
    emit(EV_ABS, ABS_MT_PRESSURE, pressure);
    emit(EV_ABS, ABS_MT_TOUCH_MAJOR, randomTouchMajor());
    return sync();
}

bool TouchInjector::touchUp() {
    if (!isReady()) { last_error_ = "设备未初始化"; return false; }
    if (state_ != TouchState::DOWN) return true;

    if (backend_ == InjectBackend::TWT_DRIVER) {
        struct twt_touch_event teb = { 0, 0, 0 };
        if (ioctl(fd_, TWT_TOUCH_UP, &teb) < 0) return false;
        state_ = TouchState::UP;
        return true;
    }

    if (backend_ == InjectBackend::KERNEL_DRIVER) {
        if (ioctl(fd_, AIM_TOUCH_UP, 0) < 0) return false;
        state_ = TouchState::UP;
        return true;
    }

    emit(EV_ABS, ABS_MT_SLOT, 0);
    emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit(EV_KEY, BTN_TOUCH, 0);
    bool ok = sync();
    if (ok) state_ = TouchState::UP;
    return ok;
}

bool TouchInjector::click(int x, int y, int duration_ms) {
    if (!touchDown(x, y)) return false;
    if (duration_ms > 0) usleep(randomJitterUs(duration_ms * 1000));
    return touchUp();
}

bool TouchInjector::swipe(int x1, int y1, int x2, int y2, int steps, int step_delay_us) {
    if (!touchDown(x1, y1)) return false;
    for (int i = 1; i <= steps; ++i) {
        int x = x1 + (x2 - x1) * i / steps;
        int y = y1 + (y2 - y1) * i / steps;
        if (!touchMove(x, y)) return false;
        if (step_delay_us > 0) usleep(randomJitterUs(step_delay_us));
    }
    return touchUp();
}

// ── 陀螺仪注入 ────────────────────────────────────────
// 向陀螺仪 input 设备写入角速度，游戏会当作物理转动处理
// 优势: 完全无触摸事件，反作弊无法通过触摸特征检测

bool TouchInjector::gyroInject(float rx, float ry, float rz) {
    if (backend_ == InjectBackend::TWT_DRIVER) {
        // TwT: 直接修改陀螺仪传感器值（GYRO_CONFIG）
        // 比 input 事件注入更底层，游戏无法通过事件特征检测
        // x, y 是 float，通过 memcpy 转为 uint32_t 传递
        struct twt_gyro_config cfg = {};
        cfg.enable = 1;
        // TwT 的 x/y 含义: x=水平转动, y=垂直转动
        // 我们的 rx=绕X轴(垂直), ry=绕Y轴(水平), rz=绕Z轴
        float twt_x = ry;  // 水平
        float twt_y = rx;  // 垂直
        memcpy(&cfg.x, &twt_x, sizeof(uint32_t));
        memcpy(&cfg.y, &twt_y, sizeof(uint32_t));
        if (ioctl(fd_, TWT_GYRO_CONFIG, &cfg) < 0) {
            last_error_ = std::string("TwT GYRO_CONFIG 失败: ") + strerror(errno);
            return false;
        }
        return true;
    }

    if (backend_ != InjectBackend::GYROSCOPE || gyro_fd_ < 0) {
        last_error_ = "陀螺仪后端未初始化";
        return false;
    }
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));

    // 将角速度（毫弧度/秒）映射到陀螺仪量程
    // 多数设备陀螺仪量程约 ±2000 dps，对应 ±32767
    // 1 dps ≈ 0.01745 rad/s
    int scale = gyro_max_ > 0 ? gyro_max_ : 32767;

    if (gyro_abs_x_ >= 0) {
        ev.type = EV_ABS; ev.code = gyro_abs_x_;
        ev.value = (int)(rx * scale / 2000.0f);
        write(gyro_fd_, &ev, sizeof(ev));
    }
    if (gyro_abs_y_ >= 0) {
        ev.type = EV_ABS; ev.code = gyro_abs_y_;
        ev.value = (int)(ry * scale / 2000.0f);
        write(gyro_fd_, &ev, sizeof(ev));
    }
    if (gyro_abs_z_ >= 0) {
        ev.type = EV_ABS; ev.code = gyro_abs_z_;
        ev.value = (int)(rz * scale / 2000.0f);
        write(gyro_fd_, &ev, sizeof(ev));
    }
    // 同步
    ev.type = EV_SYN; ev.code = SYN_REPORT; ev.value = 0;
    write(gyro_fd_, &ev, sizeof(ev));
    return true;
}

bool TouchInjector::gyroReset() {
    if (backend_ == InjectBackend::TWT_DRIVER) {
        // TwT: GYRO_CONFIG with enable=0
        struct twt_gyro_config cfg = {};
        cfg.enable = 0;
        cfg.x = 0;
        cfg.y = 0;
        return ioctl(fd_, TWT_GYRO_CONFIG, &cfg) == 0;
    }
    return gyroInject(0, 0, 0);
}

void TouchInjector::destroy() {
    if (state_ == TouchState::DOWN) touchUp();
    if (fd_ >= 0) {
        if (backend_ == InjectBackend::UINPUT) ioctl(fd_, UI_DEV_DESTROY);
        close(fd_);
        fd_ = -1;
    }
    if (gyro_fd_ >= 0) {
        close(gyro_fd_);
        gyro_fd_ = -1;
    }
    backend_ = InjectBackend::NONE;
}

} // namespace aimbot
