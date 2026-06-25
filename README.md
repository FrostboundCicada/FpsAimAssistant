# FpsAimAssistant

纯离线 Android FPS 游戏 AI 辅助瞄准工程（Kotlin + JNI + NCNN）。

## 核心特性

- **视觉推理**: NCNN 部署 YOLOv8n，支持 CPU(ARM Neon) / GPU(Vulkan)，目标延迟 <50ms
- **屏幕捕获**: SurfaceControl（低延迟，无需授权）或 MediaProjection（兼容）
- **多后端触摸注入**（反检测分级）:
  - 内核驱动（aim_touch/zero/qx/rt）— 直接调用 `input_handle_event`，最难检测
  - 陀螺仪注入 — 无触摸事件，模拟设备转动
  - uinput — 加固伪装版（BUS_I2C + OEM 名称）
- **人性化瞄准轨迹**: 贝塞尔曲线 + ease-in-out + 超调回弹 + 微颤
- **动态速度调整**: 近距离减速避免过冲（参考 RookieAI）
- **跳变检测**: 防止目标切换导致准星瞬移被检测
- **自动开火检测**: 输入事件监听 + 亮度尖峰检测

## 工程结构

```
FpsAimAssistant/
├── app/src/main/
│   ├── cpp/                    # JNI + C++ 核心
│   │   ├── input/touch_injector.{h,cpp}   # 多后端注入
│   │   ├── ncnn/yolo_v8.{h,cpp}           # YOLOv8n 推理
│   │   ├── aimbot/aimbot_controller.{h,cpp} # 瞄准控制
│   │   └── aimbot/target_tracker.{h,cpp}  # 目标跟踪
│   ├── java/com/aimassistant/  # Kotlin 层
│   └── assets/models/          # 放 yolov8n.param/.bin
└── kernel/touch_driver.c       # 独立 .ko 内核驱动
```

## 参考来源

- **RookieAI (yxdaili)**: 动态速度算法、跳变检测、像素→角度转换
- **FrostboundCicada/ovo**: 内核态 `input_handle_event` 注入方案
- **FrostboundCicada/AimBuddy**: 并行触摸流架构

## 构建前准备

1. NCNN 库: 下载 `ncnn-android-vulkan` 到 `app/src/main/cpp/ncnn-android-vulkan/`
2. 模型: `yolo export model=yolov8n.pt format=ncnn`，放入 `assets/models/`
3. Gradle Wrapper: `gradle wrapper --gradle-version 8.7`
4. 设备已 Root

## 内核驱动（可选，最强反检测）

```bash
cd kernel
make KDIR=/path/to/kernel-source
insmod aim_touch.ko
chmod 666 /dev/aim_touch
```

应用会自动检测 `/dev/aim_touch`、`/dev/zero_touch`、`/dev/qx_touch`、`/dev/rt_touch` 等已知驱动。
