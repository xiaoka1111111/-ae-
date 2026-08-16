/*
 * gl_renderer.h — GPU 渲染器 (OpenGL 3.3 Core, 隐藏窗口)
 * 管线: 噪声(simplex FBM) -> Sobel -> JFA 距离场 -> 预设图层合成
 * 与 CPU 路径 (dissolve_core/dissolve_styles) 数学一致, 用于 AE 内 GPU 渲染
 */
#pragma once
#include <cstdint>
#include <vector>
#include "preset_data.h"

namespace glr {

struct GPUBuffers;  // 不透明

// 初始化/销毁 (创建隐藏窗口 + GL 上下文)
bool init();
void shutdown();
bool available();

// 日志回调 (AE 内 printf 不可见; 插件挂接 logMsg 写入 %TEMP%\TestFill_debug.log)
typedef void (*LogFn)(const char* msg);
void setLogFn(LogFn fn);

// 渲染一帧:
//   srcRGBA: 源图像 (RGBA float, w*h*4)
//   preset:  预设
//   progress: 预设进度 0-100
//   noiseParams: 噪声参数 (索引与 Params 对应)
//   speedInfluence/borderInfluence/gamma/exposure: Fill_GPU 参数
//   speedMapChannel: 速度图通道 (-1=距离场, 0=Luma, 1=R, 2=G, 3=B, 4=A)
//   seedMask: 圆点笔刷种子掩码 (可选; nullptr = 自动选点)
//   blendMode: 面板"混合模式"覆盖 (0=跟随预设; 1..10=覆盖, 与 CPU renderPreset 同步)
//   outRGBA: 输出图层合成结果 (RGBA float) — 最终合成由 CPU 完成
bool renderFrame(const float* srcRGBA, int w, int h,
                 const Preset& preset, float progress,
                 const float* noiseParams, int complexity,
                 float alphaThreshold,
                 float speedInfluence, float borderInfluence,
                 float gamma, float exposure,
                 int speedMapChannel,
                 std::vector<float>& outRGBA,
                 const float* seedMask = nullptr,
                 int blendMode = 0,
                 int sourceMode = 0);

} // namespace glr
