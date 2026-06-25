// touch_driver.c — 独立内核触摸注入驱动（.ko，非 KPM）
//
// 通过 insmod 加载，不依赖 SukiSU/KPM 框架，兼容所有 Root 方案
// （Magisk、KernelSU、SukiSU 等）。
//
// 原理（参考 ovo/touch.c、ZeroDriver、QX 驱动）:
//   1. 遍历 input_dev_list 找到真实触摸屏的 input_dev
//   2. 直接调用 input_handle_event() 向真实设备注入事件
//   3. 注册字符设备 /dev/aim_touch 供用户态 ioctl 调用
//
// 反检测优势:
//   - 不创建虚拟设备，/proc/bus/input/devices 无可疑条目
//   - 事件来自真实触摸屏 input_dev，反作弊扫描无法区分
//   - 独立 .ko，可打包为 Magisk 模块开机自动加载
//
// 加载: insmod aim_touch.ko
// 卸载: rmmod aim_touch
//
// 构建: 需对应设备的内核源码树
//   make -C /path/to/kernel M=$(pwd) modules
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/version.h>

#define DRIVER_NAME "aim_touch"
#define DEVICE_NAME "aim_touch"
#define CLASS_NAME  "aim_touch"

// ── ioctl 命令（与用户态 uinput_touch.cpp 一致）──────────
#define AIM_TOUCH_MAGIC 'A'
struct aim_touch_point {
    int x;
    int y;
    int pressure;
    int touch_major;
    int tracking_id;
};
struct aim_touch_init {
    int screen_w;
    int screen_h;
};
#define AIM_TOUCH_DOWN   _IOW(AIM_TOUCH_MAGIC, 1, struct aim_touch_point)
#define AIM_TOUCH_MOVE   _IOW(AIM_TOUCH_MAGIC, 2, struct aim_touch_point)
#define AIM_TOUCH_UP     _IO(AIM_TOUCH_MAGIC, 3)
#define AIM_TOUCH_INIT   _IOW(AIM_TOUCH_MAGIC, 4, struct aim_touch_init)

// ── 内核符号查找 ──────────────────────────────────────
// 通过 kallsyms 查找 input_handle_event 和 input_dev_list
// 需要内核开启 CONFIG_KALLSYMS（绝大多数设备默认开启）
static int (*p_input_handle_event)(struct input_dev*, unsigned int, unsigned int, int) = NULL;
static struct list_head* p_input_dev_list = NULL;
static struct mutex* p_input_mutex = NULL;

static struct input_dev* cached_touch_dev = NULL;
static int screen_width = 1080;
static int screen_height = 2400;
static int current_tracking_id = -1;
static int current_slot = 0;
static DEFINE_MUTEX(inject_lock);

// kallsyms_lookup_name 在新内核中不再导出，用 kprobes 绕过
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
#include <linux/kprobes.h>
static struct kprobe kp = {
    .symbol_name = "kallsyms_lookup_name"
};

static unsigned long (*lookup_name)(const char* name) = NULL;

static int resolve_lookup_name(void) {
    if (lookup_name) return 0;
    int ret = register_kprobe(&kp);
    if (ret < 0) {
        pr_err("[%s] register_kprobe 失败: %d\n", DRIVER_NAME, ret);
        return ret;
    }
    lookup_name = (unsigned long (*)(const char*))kp.addr;
    unregister_kprobe(&kp);
    return 0;
}
#else
static unsigned long (*lookup_name)(const char*) = kallsyms_lookup_name;
static int resolve_lookup_name(void) { return 0; }
#endif

// ── 解析内核符号 ──────────────────────────────────────
static int resolve_symbols(void) {
    int ret = resolve_lookup_name();
    if (ret) return ret;

    if (!p_input_handle_event) {
        p_input_handle_event = (void*)lookup_name("input_handle_event");
        if (!p_input_handle_event) {
            pr_err("[%s] 无法找到 input_handle_event\n", DRIVER_NAME);
            return -ENOENT;
        }
    }
    if (!p_input_dev_list) {
        p_input_dev_list = (void*)lookup_name("input_dev_list");
    }
    if (!p_input_mutex) {
        p_input_mutex = (void*)lookup_name("input_mutex");
    }
    if (!p_input_dev_list || !p_input_mutex) {
        pr_err("[%s] 无法找到 input_dev_list/input_mutex\n", DRIVER_NAME);
        return -ENOENT;
    }
    return 0;
}

// ── 查找真实触摸屏设备 ─────────────────────────────────
static struct input_dev* find_touch_device(void) {
    struct input_dev* dev;

    if (cached_touch_dev) return cached_touch_dev;
    if (!p_input_dev_list || !p_input_mutex) return NULL;

    mutex_lock(p_input_mutex);
    list_for_each_entry(dev, p_input_dev_list, node) {
        // 真实触摸屏: 支持 EV_ABS + ABS_MT_POSITION_X，且非虚拟设备
        if (test_bit(EV_ABS, dev->evbit) &&
            test_bit(ABS_MT_POSITION_X, dev->absbit) &&
            dev->id.bustype != BUS_VIRTUAL) {
            pr_info("[%s] 找到触摸设备: %s (bus=%d vendor=0x%x product=0x%x)\n",
                    DRIVER_NAME, dev->name, dev->id.bustype,
                    dev->id.vendor, dev->id.product);
            cached_touch_dev = dev;
            mutex_unlock(p_input_mutex);
            return dev;
        }
    }
    mutex_unlock(p_input_mutex);
    pr_err("[%s] 未找到真实触摸屏\n", DRIVER_NAME);
    return NULL;
}

// ── 注入单个事件 ──────────────────────────────────────
static inline void inject_event(struct input_dev* dev, unsigned int type,
                                 unsigned int code, int value) {
    if (p_input_handle_event && dev)
        p_input_handle_event(dev, type, code, value);
}

// ── 触摸操作 ──────────────────────────────────────────
static int touch_down(struct aim_touch_point* pt) {
    struct input_dev* dev = find_touch_device();
    if (!dev) return -ENODEV;

    mutex_lock(&inject_lock);
    current_tracking_id = pt->tracking_id;
    inject_event(dev, EV_ABS, ABS_MT_SLOT, current_slot);
    inject_event(dev, EV_ABS, ABS_MT_TRACKING_ID, current_tracking_id);
    inject_event(dev, EV_ABS, ABS_MT_POSITION_X, pt->x);
    inject_event(dev, EV_ABS, ABS_MT_POSITION_Y, pt->y);
    inject_event(dev, EV_ABS, ABS_MT_PRESSURE, pt->pressure);
    inject_event(dev, EV_ABS, ABS_MT_TOUCH_MAJOR, pt->touch_major);
    inject_event(dev, EV_KEY, BTN_TOUCH, 1);
    inject_event(dev, EV_SYN, SYN_REPORT, 0);
    mutex_unlock(&inject_lock);
    return 0;
}

static int touch_move(struct aim_touch_point* pt) {
    struct input_dev* dev = find_touch_device();
    if (!dev) return -ENODEV;

    mutex_lock(&inject_lock);
    inject_event(dev, EV_ABS, ABS_MT_SLOT, current_slot);
    inject_event(dev, EV_ABS, ABS_MT_POSITION_X, pt->x);
    inject_event(dev, EV_ABS, ABS_MT_POSITION_Y, pt->y);
    inject_event(dev, EV_ABS, ABS_MT_PRESSURE, pt->pressure);
    inject_event(dev, EV_ABS, ABS_MT_TOUCH_MAJOR, pt->touch_major);
    inject_event(dev, EV_SYN, SYN_REPORT, 0);
    mutex_unlock(&inject_lock);
    return 0;
}

static int touch_up(void) {
    struct input_dev* dev = find_touch_device();
    if (!dev) return -ENODEV;

    mutex_lock(&inject_lock);
    inject_event(dev, EV_ABS, ABS_MT_SLOT, current_slot);
    inject_event(dev, EV_ABS, ABS_MT_TRACKING_ID, -1);
    inject_event(dev, EV_KEY, BTN_TOUCH, 0);
    inject_event(dev, EV_SYN, SYN_REPORT, 0);
    current_tracking_id = -1;
    mutex_unlock(&inject_lock);
    return 0;
}

// ── ioctl 处理 ───────────────────────────────────────
static long aim_touch_ioctl(struct file* f, unsigned int cmd, unsigned long arg) {
    switch (cmd) {
    case AIM_TOUCH_INIT: {
        struct aim_touch_init init;
        if (copy_from_user(&init, (void*)arg, sizeof(init))) return -EFAULT;
        screen_width = init.screen_w;
        screen_height = init.screen_h;
        if (resolve_symbols() != 0) return -ENOENT;
        pr_info("[%s] 初始化 %dx%d\n", DRIVER_NAME, screen_width, screen_height);
        return 0;
    }
    case AIM_TOUCH_DOWN: {
        struct aim_touch_point pt;
        if (copy_from_user(&pt, (void*)arg, sizeof(pt))) return -EFAULT;
        return touch_down(&pt);
    }
    case AIM_TOUCH_MOVE: {
        struct aim_touch_point pt;
        if (copy_from_user(&pt, (void*)arg, sizeof(pt))) return -EFAULT;
        return touch_move(&pt);
    }
    case AIM_TOUCH_UP:
        return touch_up();
    default:
        return -EINVAL;
    }
}

// ── 字符设备 ─────────────────────────────────────────
static struct cdev aim_cdev;
static struct class* aim_class;
static struct device* aim_device;
static dev_t aim_devno;

static const struct file_operations aim_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = aim_touch_ioctl,
    .compat_ioctl = aim_touch_ioctl,
};

static int __init aim_touch_init(void) {
    int ret;

    ret = alloc_chrdev_region(&aim_devno, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("[%s] alloc_chrdev_region 失败: %d\n", DRIVER_NAME, ret);
        return ret;
    }

    cdev_init(&aim_cdev, &aim_fops);
    aim_cdev.owner = THIS_MODULE;
    ret = cdev_add(&aim_cdev, aim_devno, 1);
    if (ret < 0) {
        unregister_chrdev_region(aim_devno, 1);
        return ret;
    }

    aim_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(aim_class)) {
        cdev_del(&aim_cdev);
        unregister_chrdev_region(aim_devno, 1);
        return PTR_ERR(aim_class);
    }

    aim_device = device_create(aim_class, NULL, aim_devno, NULL, DEVICE_NAME);
    if (IS_ERR(aim_device)) {
        class_destroy(aim_class);
        cdev_del(&aim_cdev);
        unregister_chrdev_region(aim_devno, 1);
        return PTR_ERR(aim_device);
    }

    // 设置权限 666
    // 通过 sysfs 在 module_param 中设置，或 init 脚本 chmod
    pr_info("[%s] 驱动加载成功 /dev/%s (major=%d)\n",
            DRIVER_NAME, DEVICE_NAME, MAJOR(aim_devno));
    pr_info("[%s] 用法: chmod 666 /dev/%s && 应用自动检测\n",
            DRIVER_NAME, DEVICE_NAME);
    return 0;
}

static void __exit aim_touch_exit(void) {
    device_destroy(aim_class, aim_devno);
    class_destroy(aim_class);
    cdev_del(&aim_cdev);
    unregister_chrdev_region(aim_devno, 1);
    pr_info("[%s] 驱动卸载\n", DRIVER_NAME);
}

module_init(aim_touch_init);
module_exit(aim_touch_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("AimAssistant");
MODULE_DESCRIPTION("Standalone kernel touch injection driver (anti-detection)");
MODULE_VERSION("1.0");
