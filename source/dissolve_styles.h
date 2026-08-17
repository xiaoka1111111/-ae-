/*
 * dissolve_styles.h — 预设多层波次渲染系统
 *
 * 预设引擎:
 *   - 每个预设 = 最多 5 个图层 (style layers)
 *   - 每层有独立时间窗口 [start,end] (0-99%)、颜色/渐变、混合模式、
 *     grow 生长速度、blur、displace 噪声位移
 *   - 波次 = 距离场从边缘向内部推进 (Reveal) 或反向 (Out)
 *
 * 混合模式编号映射到 PF_Xfer 枚举 (AE_EffectCB.h), 见 BlendMode 枚举。
 */
#pragma once
#include "dissolve_core.h"
#include "preset_data.h"

namespace dissolve {

// smoothstep 辅助
static inline float smoothstepField(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = std::min(std::max(t, 0.f), 1.f);
    return t * t * (3.f - 2.f * t);
}

// 混合模式 = 原版"叠加模式"参数值 (1-based popup 值, 0x3D830 跳表)
enum BlendMode {
    BM_NORMAL        = 1,   // PF_Xfer_IN_FRONT — src-over (默认)
    BM_MULTIPLY      = 3,   // PF_Xfer_MULTIPLY
    BM_COLOR_BURN    = 4,   // PF_Xfer_COLOR_BURN (经典: 1-(1-B)/S)
    BM_ADD           = 6,   // PF_Xfer_ADD (原版英文 Add, 中文化显示"叠加")
    BM_SCREEN        = 7,   // PF_Xfer_SCREEN
    BM_OVERLAY       = 9,   // PF_Xfer_OVERLAY
    BM_SOFT_LIGHT    = 10,  // PF_Xfer_SOFT_LIGHT
    BM_COLOR         = 12,  // PF_Xfer_COLOR
    BM_STENCIL_ALPHA = 14,  // PF_Xfer_MULTIPLY_ALPHA
    BM_STENCIL_LUMA  = 15,  // PF_Xfer_MULTIPLY_ALPHA_LUMA
    BM_SILHOU_ALPHA  = 16,  // PF_Xfer_MULTIPLY_NOT_ALPHA
    BM_SILHOU_LUMA   = 17,  // PF_Xfer_MULTIPLY_NOT_ALPHA_LUMA
};

// 图层渲染上下文 (每帧计算)
struct StyleFrame {
    const Preset* preset = nullptr;
    float progress = 0.f;      // 0-100, 预设时间进度
    float t = 0.f;             // 归一化时间 0-1 (含 repeat 循环)
    // 可选: Fill_GPU 参数 (speedOverlay/borderControl/gamma/exposure)
    // 原版: fillMap 在层渲染前经 speedOverlay(fill, 距离场) + borderControl(边缘)
    // 若为 nullptr 则跳过 Fill_GPU (行为与旧版一致)
    const Params* params = nullptr;
    // 可选: 圆点笔刷种子掩码 (原版 0x140E96, 用户放置点最多 5 个)
    // 非空时波前从掩码>0.05 的所有点同时传播 (多源 BFS), 替代自动选点
    const float* seedMask = nullptr;
};

// 圆点笔刷种子掩码 (原版 shader 0x140E96 ):
//   pts: 2*n 的点坐标 (像素); n: 点数 (1-5); radiusF: 笔刷半径 (像素)
//   软边: sq_len in ((r-1)^2, r^2] -> factor 线性衰减; 输出 [0,1] 掩码
void brushSeedMask(const float* pts, int n, float radiusF, int w, int h,
                   std::vector<float>& mask);

// 按归一化位置采样渐变色标
void sampleGradient(const PresetColorStop* stops, int n, float pos,
                    float* r, float* g, float* b, float* a);

// 混合: dst = blend(dst, src, mode) * opacity
void blendPixel(float* dstR, float* dstG, float* dstB, float* dstA,
                float srcR, float srcG, float srcB, float srcA,
                int mode, float opacity);

// 渲染预设到 fillMap (单通道强度) + colorMap (RGB), 供 final 合成
//   distField 已就绪 (波前场), noiseMap 用于 displace
//   srcRGBA: 源图层像素 (RGBA float, 可选 — mode=3 "原图显现层" 使用; nullptr 时用白色)
void renderPreset(const StyleFrame& fr,
                  const Buffers& buf, const float* srcRGBA, int w, int h,
                  std::vector<float>& colorR, std::vector<float>& colorG,
                  std::vector<float>& colorB, std::vector<float>& layerAlpha);

// 便捷: 时间 -> progress (秒, duration/repeat 循环)
float progressFromTime(const Preset& p, float seconds);

} // namespace dissolve
