/*
 * dissolve_direct.h — 直通渲染管线核心内核 (波前扩散填充)
 *
 * 本模块实现填充算法的全部基础内核:
 *   圆点 splat        — 层渲染 mode==1 的 fillMap 种子生成 (软边圆)
 *   膨胀核            — fill 场每帧 3x3 max 传播 (保留满覆盖像素)
 *   斜坡内核          — 渐变层填充值 (fill·S + 进度斜坡)
 *   overlay+lerp      — Fill_GPU 合成 (0.5 阈值 overlay + 权重 lerp)
 *
 * 渲染管线总流程见 dissolve_direct.cpp 头部与 docs/01-填充算法原理.md。
 */
#pragma once
#include <vector>
#include <cstdint>
#include "preset_data.h"

namespace dissolve {

// 圆点 splat (- 实现):
//   每层一个 16 位定点坐标点 + 阈值; 阈值 <= progress 时把软边圆画进 fillMap (max 累积)
//   cx = 点X×2^-16/除数 − ox; cy = 点Y×2^-16/除数 − oy; r = 半径参数/除数
//   包围盒 [floor(cx−r−2), ceil(cx+r+2)]×[floor(cy−r−2), ceil(cy+r+2)]
//   d² = (x−cx+0.5)²+(y−cy+0.5)²
//   d² ≤ r²: factor = d² ≤ (r−1)² ? 1.0 : (r²−d²)/(r²−(r−1)²); buf = max(buf, factor)
// pts: n*2 个点 (浮点像素坐标, 已换算); thresh: n 个阈值; radiusF: 半径
// prog01: 归一化进度 [0,1]; ox/oy: 渲染区域原点偏移 (层记录位移)
void splatFillMap(const float* pts, const float* thresh, int n,
                  float radiusF, float prog01, int w, int h,
                  float ox, float oy, std::vector<float>& fillMap);

// 膨胀核 (/ 实现): 3x3 max 滤波
//   规则: 未满像素 (值 < 1.0) 取邻域最大; 已满像素 (值 >= 1.0) 保留原值
//   (与 dilate shader  #if0 设计语义一致; 每帧 1px = 切比雪夫传播)
void dilateMaxField(const std::vector<float>& src, std::vector<float>& dst,
                    int w, int h, int iterations);

// 斜坡内核 ( 实现): 渐变层填充值
//   out = clamp(fill·S + 0.5·S·(p−0.5) + 2.5·p − 1, 0, 1)
//   fill: 填充场值; S: param 0x18 (对比度参数, 来源待定 [C]); p: 归一化进度
float rampKernel30B80(float fill, float S, float p);

// overlay+lerp (/ 实现): 合成
//   o = dst<0.5 ? 2·dst·src : 1−2(1−dst)(1−src)
//   out = lerp(o, dst, w)
float overlayLerp30E50(float dst, float src, float w);

// 四边形扭曲采样 (/ 实现, -速度图参数.md §1.4-1.5):
//   输出像素 (x,y) → 源采样坐标:
//     dst_x = P0.x + ((x−P0.x)/(P1.x−P0.x))·(P3.x−P2.x) + P2.x − dispX
//     dst_y = P0.y + ((y−P0.y)/(P1.y−P0.y))·(P3.y−P2.y) + P2.y − dispY
//   钳制到 [0,srcW)×[0,srcH); 越界 → 0
//   速度值: mode1 = R·(G+B+A)/(3·scale²); mode2 = R/scale; 阈 0.001; clamp [0,1]
// quad: {P0x,P0y, P1x,P1y, P2x,P2y, P3x,P3y} (输出区→源区映射四边形)
//   P0=源左上, P1=源右上(垂直方向?), 设计语义: P0/P1 定义 x_src 方向, P2/P3 目标
// dispX/dispY: 记录 +0x68/+0x6c 位移量 (层记录位移)
void quadWarpSpeedMap(const float* srcRGBA, int srcW, int srcH,
                      const float* quad, float dispX, float dispY,
                      int mode, float scale, int outW, int outH,
                      std::vector<float>& speedMap);

// ---------- 实现渲染管线 (GrowthDrawCPU  主流程) ----------
// 流程 (实现-渲染管线规格.md §5):
//   1. 层激活收集: 硬阈值 gating (阈值 = 层 UI 参数 30+2i; 阈值≤p01 激活)
//   2. mode 分派: mode==1 激活层圆点 splat → fillMap (max 累积)
//   3. 传播: 8 邻域 BFS 距离场 = 膨胀帧号场 (-Growth传播语义.md 证明精确等价;
//      fillMap(p01) = dist ≤ 已过帧数)
//   4. 层着色: 实色/渐变()/源图 × 填充强度
//   5. 合成: overlay+lerp  数据序 (后画覆盖先画)
struct DirectFrame {
    const Preset* preset = nullptr;
    float progress01 = 0.f;          // 归一化进度 p01 = clamp((frame+1)/divisor, 0, 1) — 层 gating 用
    float totalFrames = 100.f;       // 归一化分母 () [C]
    float explicitFrames = -1.f;     // 已过帧数 (>=0 时直接用; <0 时 = p01×totalFrames−1)
                                     //   设计 fillMap 传播 = 帧号场 (frame01 帧膨胀), 与 gating 进度独立
    float splatRadius = 10.f;        // 圆点半径 param 28 (默认 10.0@)
    float rampS = 1.f;               //  的 S = param 24 (噪波对比度)
    float divisor = 1.f;             // 除数表 {1,1,2,0.5}[param 6 质量]
    float ox = 0.f, oy = 0.f;        // 层记录位移 +0x68/+0x6c
    int   growthSource = 0;          // param 9 生长来源: 0=点 1=噪波 2=图层
                                     //   (修正: mode 分派 = 生长来源, 非层类型!)
    uint64_t staticKey = 0;          // BFS 距离场缓存键 (形状指纹 ^ 点/半径配置指纹);
                                     //   0=不缓存。距离场只依赖激活层集合 (延迟阈值离散事件),
                                     //   按 (staticKey, 激活掩码) 缓存 — 每帧仅 O(n) 阈值化
    const float* layerPts = nullptr;     // nLayers*2 点坐标 (param 29+2i, 默认 {45,45})
    const float* layerThresh = nullptr;  // nLayers 延迟阈值 (param 30+2i, 默认 0, 0-100)
    const float* srcRGBA = nullptr;      // 源图 (生长来源=图层)
    const float* noiseFill = nullptr;    // noiseMap (生长来源=噪波 的  fill)
    const float* shapeAlpha = nullptr;   // 形状 alpha (可选; 作为输出掩码把填充裁剪到图层不透明区。
                                         //   BFS 传播在全区域进行, 点位于 alpha 外时波前仍可进入形状)
    float slotWeight = 1.f;              // 槽位权重 () [C]
    // 循环包络 [B]: 周期末尾平滑淡出系数 [0,1] (1=正常)。实现循环 (fmod) 回绕时
    //   硬跳变 "满填充→1%" 观感像 bug; 85%→100% 淡出后回绕平滑。mode-3 源图层豁免
    //   (文字不能闪烁), 由 renderPresetDirect 内判断 L.mode==3。
    float loopEnv = 1.f;
    // 文字模式 (0=设计: mode-3 源图层 src-over 盖在填充上, 文字保持原色;
    //   1=填充覆盖文字: 跳过 mode-3 层, 彩色直接覆盖图层区域) [B]
    int sourceLayerMode = 0;
    // 混合模式 ( 跳表权威映射, A 级依据; 1-based popup 值, 见 dissolve_styles.h):
    //   1=正常(IN_FRONT) 3=正片叠底 4=颜色加深 6=相加 7=滤色 9=叠加 10=柔光 12=颜色
    //   14/15=模版Alpha/亮度 16/17=剪影Alpha/亮度; 0/非法值=正常
    int   blendMode = 1;
    // Fill_GPU (, 可选; 非空时对填充强度应用 speedOverlay+borderControl+gamma):
    //   9 步链 (, A 级, 实现-调试路径与fill链.md): 1 warp累加 → 2/3 高斯模糊H/V
    //   (: exp(-0.5·(x/R)²/0.06), 浮点半径 R/R+1 双核插值) → 4 gamma/exposure
    //   (: v>0.0001 ? clamp(pow(v,γ)·exp,0,1) : 0) → 5 speedOverlay ()
    //   → 6-8 清零/拷贝/膨胀(缓冲管线, 单场 max 累积等价) → 9 borderControl
    //   (: dst·(1−w·(1−src)))
    const float* speedMap = nullptr;     // 速度图 ( 的 src)
    float speedMapInfluenceF = 0.f;
    const float* edgeMap = nullptr;      // 边框图 (Sobel )
    float borderInfluenceF = 0.f;
    float blurRadius = 0.f;              // 链 2/3 步高斯模糊半径 (param 96 模糊, 默认 0=关)
    float gammaF = 1.f;
    float exposureF = 1.f;
};

void renderPresetDirect(const DirectFrame& fr, int w, int h,
                        std::vector<float>& colorR, std::vector<float>& colorG,
                        std::vector<float>& colorB, std::vector<float>& layerAlpha);

} // namespace dissolve
