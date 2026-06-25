# CMakeLists.txt — FpsAimAssistant native layer
# Builds: uinput 触摸注入 + NCNN YOLOv8n 推理 + 瞄准控制器
cmake_minimum_required(VERSION 3.22.1)
project("aimassistant")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─────────────────────────────────────────────────────────────
# NCNN 预编译库位置（Vulkan 版本）。
# 下载地址: https://github.com/Tencent/ncnn/releases
# 解压后将目录重命名为 ncnn-android-vulkan 并放入本 cpp 目录。
# 目录结构:
#   cpp/ncnn-android-vulkan/
#     ├── include/ncnn/
#     └── lib/arm64-v8a/libncnn.a
# ─────────────────────────────────────────────────────────────
set(NCNN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ncnn-android-vulkan)

# ─────────────────────────────────────────────────────────────
# 源文件
# ─────────────────────────────────────────────────────────────
set(SRC_LIST
    jni_bridge.cpp
    input/touch_injector.cpp
    ncnn/yolo_v8.cpp
    aimbot/target_tracker.cpp
    aimbot/aimbot_controller.cpp
)

add_library(${PROJECT_NAME} SHARED ${SRC_LIST})

# ─────────────────────────────────────────────────────────────
# 头文件路径
# ─────────────────────────────────────────────────────────────
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${NCNN_DIR}/include
)

# ─────────────────────────────────────────────────────────────
# 链接 Android 系统库
# ─────────────────────────────────────────────────────────────
find_library(log-lib log)
find_library(android-lib android)

# Vulkan（GPU 推理后端）
find_library(vulkan-lib vulkan)

target_link_libraries(${PROJECT_NAME}
    ${log-lib}
    ${android-lib}
    ${vulkan-lib}
)

# ─────────────────────────────────────────────────────────────
# NCNN 静态库（若存在则链接，否则编译期给出友好提示）
# ─────────────────────────────────────────────────────────────
if(EXISTS "${NCNN_DIR}/lib/arm64-v8a/libncnn.a")
    target_link_libraries(${PROJECT_NAME} ${NCNN_DIR}/lib/arm64-v8a/libncnn.a)
    message(STATUS "NCNN: 已找到预编译库，启用 NCNN 推理")
else()
    message(WARNING "NCNN: 未找到 ${NCNN_DIR}/lib/arm64-v8a/libncnn.a，"
                    "请下载 ncnn-android-vulkan 并放入 cpp/ncnn-android-vulkan/")
endif()

# ─────────────────────────────────────────────────────────────
# 编译选项：开启 NEON 优化、隐藏符号
# ─────────────────────────────────────────────────────────────
target_compile_options(${PROJECT_NAME} PRIVATE
    -O3
    -ffast-math
    -mfpu=neon
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -Wall
    -Wno-unused-parameter
)
