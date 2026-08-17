/*
 * dissolve_direct.cpp — 直通渲染管线内核实现
 * 每个函数职责见 dissolve_direct.h 头注释; 渲染主流程见 renderPresetDirect。
 */
#include "dissolve_direct.h"
#include "dissolve_styles.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <queue>
#include <mutex>
#include <unordered_map>

namespace dissolve {

// BFS 距离场缓存 (每帧仅 O(n) 阈值化; 距离场只依赖激活层集合):
//   key = 配置指纹×31 + 激活掩码; 延迟阈值是离散事件 → 掩码数 ≤ 2^nLayers
//   AE 多线程渲染 → 互斥锁保护
static std::unordered_map<uint64_t, std::pair<int, std::vector<float>>> g_chebCache;
static std::mutex g_chebMutex;

void splatFillMap(const float* pts, const float* thresh, int n,
                  float radiusF, float prog01, int w, int h,
                  float ox, float oy, std::vector<float>& fillMap)
{
    if (n <= 0 || !pts || w <= 0 || h <= 0) return;
    if (fillMap.size() != (size_t)w * h) fillMap.assign((size_t)w * h, 0.f);

    const float r = radiusF;                 // 半径 (原版 [rbp-0x10]/除数)
    const float r2 = r * r;                  // sq_rad
    const float m2 = (r - 1.f) * (r - 1.f);  // sq_margin (软边内界)

    for (int k = 0; k < n; k++) {
        float th = thresh ? thresh[k] : 0.f;
        if (th > prog01) continue;  // 硬阈值 gating (0x18CD0: 阈值 > progress 跳过)
        float cx = pts[k*2+0] - ox;  // 圆点中心 (点×2^-16/除数 − ox)
        float cy = pts[k*2+1] - oy;

        // 包围盒 [floor(cx−r−2), ceil(cx+r+2)] (0x193xx)
        int x0 = std::max((int)std::floor(cx - r - 2.f), 0);
        int y0 = std::max((int)std::floor(cy - r - 2.f), 0);
        int x1 = std::min((int)std::ceil(cx + r + 2.f), w - 1);
        int y1 = std::min((int)std::ceil(cy + r + 2.f), h - 1);
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                // d² = (x−cx+0.5)²+(y−cy+0.5)² (0.5 像素中心偏移, 0x195xx)
                float dx = (float)x - cx + 0.5f;
                float dy = (float)y - cy + 0.5f;
                float d2 = dx * dx + dy * dy;
                if (d2 > r2) continue;
                float factor = 1.f;
                if (d2 > m2) {
                    factor = (r2 - d2) / (r2 - m2);  // 软边线性衰减 (0x195A7-0x195AF)
                }
                size_t i = (size_t)y * w + x;
                if (factor > fillMap[i]) fillMap[i] = factor;  // max 累积
            }
        }
    }
}

void dilateMaxField(const std::vector<float>& src, std::vector<float>& dst,
                    int w, int h, int iterations)
{
    size_t n = (size_t)w * h;
    if (src.size() != n) return;
    dst = src;
    if (iterations <= 0 || w <= 0 || h <= 0) return;
    std::vector<float> tmp(n);
    for (int it = 0; it < iterations; it++) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                size_t i = (size_t)y * w + x;
                float cur = dst[i];
                if (cur >= 1.f) { tmp[i] = cur; continue; }  // 已满像素保留 (0x36D7D)
                float best = 0.f;
                for (int dy = -1; dy <= 1; dy++) {
                    int py = y + dy;
                    if (py < 0 || py >= h) continue;
                    for (int dx = -1; dx <= 1; dx++) {
                        int px = x + dx;
                        if (px < 0 || px >= w) continue;
                        float v = dst[(size_t)py * w + px];
                        if (v > best) best = v;  // 邻域最大 (阈值化比较, thresh≈0)
                    }
                }
                tmp[i] = best;
            }
        }
        dst.swap(tmp);
    }
}

float rampKernel30B80(float fill, float S, float p)
{
    // out = clamp(fill·S + 0.5·S·(p−0.5) + 2.5·p − 1, 0, 1)  (0x30CB4-0x30CD9)
    // 常量: 0.5@0x13C654, 1.5@0x13C6C4, 1.0@0x13C694
    float v = fill * S + 0.5f * S * (p - 0.5f) + 2.5f * p - 1.f;
    return std::min(std::max(v, 0.f), 1.f);
}

float overlayLerp30E50(float dst, float src, float w)
{
    // o = dst<0.5 ? 2·dst·src : 1−2(1−dst)(1−src)  (0.5 阈值, 0x13C654)
    // out = lerp(o, dst, w)
    float o = dst < 0.5f ? (2.f * dst * src)
                         : (1.f - 2.f * (1.f - dst) * (1.f - src));
    return o * w + dst * (1.f - w);
}

// 高斯模糊 (fill 链 2/3 步, 0x24F50 , A 级):
//   权重 exp(-0.5·(i/R)²/0.06) (常量 -0.5@0x13C788, 0.06@0x13C6A8), 全抽头求和归一化;
//   浮点半径: R=trunc(radius), R+1 两个整数核结果按 frac(radius) 插值
//   (0x24F50: 两遍 0x1294 建核 — 半径 R 与 R+1 — 权重和求倒数归一化);
//   边界 clamp-to-edge (原版纹理采样语义)
static void gaussBlurField24F50(std::vector<float>& field, int w, int h, float radius)
{
    if (radius < 0.5f) return;
    int r1 = (int)std::floor(radius);
    int r2 = r1 + 1;
    float alpha = radius - (float)r1;  // fract(radius)
    const size_t n = (size_t)w * h;
    std::vector<float> h1(n), h2(n), v1(n), v2(n);
    auto blur1d = [&](std::vector<float>& dst, const std::vector<float>& src,
                      bool horiz, int rL) {
        if (rL <= 0) { dst = src; return; }
        const float invR = 1.0f / (float)rL;
        // 权重预计算 (全抽头, 与 0x24F50 逐位一致)
        std::vector<float> wgt((size_t)2 * rL + 1);
        float tw = 0.f;
        for (int i = -rL; i <= rL; i++) {
            float xi = (float)i * invR;
            wgt[(size_t)i + rL] = std::exp(-0.5f * (xi * xi) / 0.06f);
            tw += wgt[(size_t)i + rL];
        }
        for (size_t i = 0; i < wgt.size(); i++) wgt[i] /= tw;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                float acc = 0.f;
                for (int i = -rL; i <= rL; i++) {
                    int px = horiz ? x + i : x;
                    int py = horiz ? y : y + i;
                    if (px < 0) px = 0; if (px >= w) px = w - 1;
                    if (py < 0) py = 0; if (py >= h) py = h - 1;
                    acc += wgt[(size_t)i + rL] * src[(size_t)py * w + px];
                }
                dst[(size_t)y * w + x] = acc;
            }
        }
    };
    blur1d(h1, field, true, r1);
    blur1d(h2, field, true, r2);
    for (size_t i = 0; i < n; i++)
        field[i] = h1[i] * (1.f - alpha) + h2[i] * alpha;
    blur1d(v1, field, false, r1);
    blur1d(v2, field, false, r2);
    for (size_t i = 0; i < n; i++)
        field[i] = v1[i] * (1.f - alpha) + v2[i] * alpha;
}

void quadWarpSpeedMap(const float* srcRGBA, int srcW, int srcH,
                      const float* quad, float dispX, float dispY,
                      int mode, float scale, int outW, int outH,
                      std::vector<float>& speedMap)
{
    speedMap.assign((size_t)outW * outH, 0.f);
    if (!srcRGBA || !quad || srcW <= 0 || srcH <= 0 || scale <= 0.f) return;
    // quad: {P0x,P0y, P1x,P1y, P2x,P2y, P3x,P3y}
    float p0x = quad[0], p0y = quad[1], p1x = quad[2], p1y = quad[3];
    float p2x = quad[4], p2y = quad[5], p3x = quad[6], p3y = quad[7];
    float dx1 = p1x - p0x, dy1 = p1y - p0y;  // x_src 方向
    float dx3 = p3x - p2x, dy3 = p3y - p2y;  // 目标方向
    const float invScale2 = 1.f / (scale * scale);
    const float invScale = 1.f / scale;

    for (int y = 0; y < outH; y++) {
        for (int x = 0; x < outW; x++) {
            // 四边形插值 (0x33340-0x333C5)
            float fx = dx1 != 0.f ? ((float)x - p0x) / dx1 : 0.f;
            float fy = dy1 != 0.f ? ((float)y - p0y) / dy1 : 0.f;
            float sx = p0x + fx * dx3 + p2x - dispX;
            float sy = p0y + fy * dy3 + p2y - dispY;
            int ix = (int)sx, iy = (int)sy;
            if (ix < 0 || iy < 0 || ix >= srcW || iy >= srcH) continue;  // 越界 0
            size_t si = (size_t)iy * srcW + ix;
            float R = srcRGBA[si*4+0], G = srcRGBA[si*4+1];
            float B = srcRGBA[si*4+2], A = srcRGBA[si*4+3];
            float val;
            if (mode == 1) {
                // R·(G+B+A)/(3·scale²) (0x33423-0x33491, 归一化 3.0@0x13C724)
                val = (R * (G + B + A) / 3.f) * invScale2;
            } else if (mode == 2) {
                val = R * invScale;
            } else {
                val = 0.f;  // 无第三模式 (实证)
            }
            if (val < 0.001f) val = 0.f;  // 阈 0.001@0x13C638
            val = std::min(std::max(val, 0.f), 1.f);
            speedMap[(size_t)y * outW + x] = val;
        }
    }
}

// ---------- 渲染管线 (GrowthDrawCPU 0x18060 主流程) ----------

// 8 邻域 BFS 距离场 (切比雪夫) — 与逐帧 3x3 max 膨胀精确等价
// (Growth传播语义.md §0.1: 每帧 1px 膨胀 = 切比雪夫距离; 帧号场 = 距离场)
// 形状限制 (shapeAlpha, 可选): 传播只在 shapeAlpha>th 的像素
//   (dilate shader 0x144801 #if0 cull: fill>0.002 裁剪; 原版效果 = 图层内容形状上
//    演化动画, 非全图填充 — 用户实证"半透明图层底+动画在图层上")
static void bfsChebyshev(const std::vector<float>& seed, int w, int h,
                         std::vector<float>& dist,
                         const float* shapeAlpha = nullptr, float th = 0.05f)
{
    size_t n = (size_t)w * h;
    dist.assign(n, 1e9f);
    std::vector<int> qx, qy;
    qx.reserve(n / 4); qy.reserve(n / 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            if (shapeAlpha && shapeAlpha[i] <= th) continue;  // 形状外不传播
            if (seed[i] > 0.f) { dist[i] = 0.f; qx.push_back(x); qy.push_back(y); }
        }
    const int dx[8] = { 1,-1, 0, 0, 1, 1,-1,-1 };
    const int dy[8] = { 0, 0, 1,-1, 1,-1, 1,-1 };
    for (size_t qi = 0; qi < qx.size(); qi++) {
        int x = qx[qi], y = qy[qi];
        float d = dist[(size_t)y * w + x];
        for (int k = 0; k < 8; k++) {
            int nx = x + dx[k], ny = y + dy[k];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            size_t ni = (size_t)ny * w + nx;
            if (shapeAlpha && shapeAlpha[ni] <= th) continue;  // 形状外不传播
            if (dist[ni] > d + 1.f) {
                dist[ni] = d + 1.f;
                qx.push_back(nx); qy.push_back(ny);
            }
        }
    }
}

void renderPresetDirect(const DirectFrame& fr, int w, int h,
                        std::vector<float>& colorR, std::vector<float>& colorG,
                        std::vector<float>& colorB, std::vector<float>& layerAlpha)
{
    size_t n = (size_t)w * h;
    colorR.assign(n, 0.f); colorG.assign(n, 0.f); colorB.assign(n, 0.f);
    layerAlpha.assign(n, 0.f);
    if (!fr.preset || w <= 0 || h <= 0) return;

    const Preset& pres = *fr.preset;
    int nLayers = std::min(pres.nLayers, 5);
    float p01 = std::min(std::max(fr.progress01, 0.f), 1.f);

    // ---- 层参数 (param 29+2i 点坐标 默认 {45,45}; param 30+2i 延迟 默认 0) ----
    // 参数来源.md §1.1: 位置 {45,45} (0x2d0000), 延迟 0-100 滑块默认 0
    std::vector<float> thresh(nLayers, 0.f);
    for (int li = 0; li < nLayers; li++) {
        // 层 gating 阈值 = 点延迟 param 30+2i (默认 0)。
        // 主控复核 F2 (2026-08-16): 预设 start/end (参数 123/124) 仅由预设 I/O 读写,
        //   逐帧渲染路径 (0x26780/0x18060/0x1D4B0) 无消费点 — 不映射 start!
        if (fr.layerThresh) thresh[li] = fr.layerThresh[li];
        else thresh[li] = 0.f;  // 原版默认延迟 0 = 所有层立即激活
    }
    std::vector<float> pts((size_t)nLayers * 2, 0.f);
    for (int li = 0; li < nLayers; li++) {
        if (fr.layerPts) { pts[(size_t)li*2+0] = fr.layerPts[(size_t)li*2+0];
                           pts[(size_t)li*2+1] = fr.layerPts[(size_t)li*2+1]; }
        else { pts[(size_t)li*2+0] = 45.f;   // 原版默认 {45,45} (A 级)
               pts[(size_t)li*2+1] = 45.f; }
    }

    // ---- 1. fillMap 生成: 生长来源分派 (param 9 = 生长来源, A 级修正) ----
    // 统一模型 [重构 2026-08-18]: 种子场 → BFS 掩码内传播 → baseFill。
    //   种子按生长来源构建 (原版三种来源都是"起始点"):
    //     0=点:   splat 圆 (延迟 gating)
    //     1=噪波: 0x30B80 ramp(噪声场) > 0.01 的像素 — 噪波白区作起始点;
    //             ramp 含 +2.5p−1 进度项 → 种子随生长逐步出现 ("一边生长一边出现")
    //     2=图层: 源图 luma > 0.05 的像素 (外部图层 alpha/luma 作起始点)
    //   传播掩码 (fr.propMask, 原版 Borders): 波前只能在掩码内流动; 缺省 = shapeAlpha。
    //   种子位于掩码外时传播被阻断 (原版: 点应置于不透明区域才生效)。
    std::vector<float> fillMap(n, 0.f);
    std::vector<float> baseFill(n, 0.f);
    std::vector<float> ndF;   // 归一化距离场 (0=最近内容, 1=最远; 形状外=2.0)
    float minD = 0.f, maxD = 1.f;  // 内容距离范围 (层循环也需要)
    {
        // 传播边界与显示裁剪共用同一掩码: 桥接/弱边界时掩码已膨胀 →
        // 溢出区域可见填充 (原版 Border Strength 低值溢出语义)
        const float* pmask = fr.propMask ? fr.propMask : fr.shapeAlpha;
        const float* dmask = fr.propMask ? fr.propMask : fr.shapeAlpha;
        // ---- 激活掩码: 延迟阈值是离散事件 (thresh[li] ≤ p01), 掩码变化才需重算距离场 ----
        int activeMask = 0;
        for (int li = 0; li < nLayers; li++)
            if (thresh[li] <= p01) activeMask |= (1 << li);
        // 缓存键: 形状/点配置指纹 (调用方) + 除数/位移 (DirectFrame 配置);
        // 噪波/图层生长种子随时间/源图变化 → 不缓存 (key=0)
        uint64_t cfg = (fr.growthSource == 0) ? fr.staticKey : 0ULL;
        if (cfg) {
            uint32_t d; float f;
            f = fr.divisor; std::memcpy(&d, &f, 4); cfg = cfg * 0x9E3779B97F4A7C15ULL + d;
            f = fr.ox;      std::memcpy(&d, &f, 4); cfg = cfg * 0x9E3779B97F4A7C15ULL + d;
            f = fr.oy;      std::memcpy(&d, &f, 4); cfg = cfg * 0x9E3779B97F4A7C15ULL + d;
        }
        uint64_t key = cfg ? (cfg * 31ULL + (uint64_t)activeMask) : 0ULL;

        std::vector<float> dist;
        bool cached = false;
        if (key) {
            std::lock_guard<std::mutex> lk(g_chebMutex);
            auto it = g_chebCache.find(key);
            if (it != g_chebCache.end() && it->second.first == (int)n) {
                dist = it->second.second;
                cached = true;
            }
        }
        if (!cached) {
            // ---- 种子场构建 (按生长来源) ----
            std::vector<float> seed(n, 0.f);
            if (fr.growthSource == 0) {
                const float divs = std::max(fr.divisor, 0.001f);
                for (int li = 0; li < nLayers; li++) {
                    if (thresh[li] > p01) continue;  // 硬阈值: 延迟 > 进度 → 跳过
                    float p2[2] = { pts[(size_t)li*2+0] / divs, pts[(size_t)li*2+1] / divs };
                    splatFillMap(p2, &thresh[li], 1, fr.splatRadius / divs, p01,
                                 w, h, fr.ox, fr.oy, seed);
                }
            } else if (fr.growthSource == 1) {
                // 噪波白区作起始点: ramp(噪声场, S, p01) > 0.01
                for (size_t i = 0; i < n; i++) {
                    if (pmask && pmask[i] <= 0.05f) continue;  // 掩码外不作种子
                    float fillV = fr.noiseFill ? fr.noiseFill[i] : 0.5f;
                    if (rampKernel30B80(fillV, fr.rampS, p01) > 0.01f) seed[i] = 1.f;
                }
            } else {  // growthSource == 2: 源图 luma 白区作起始点
                for (size_t i = 0; i < n; i++) {
                    if (pmask && pmask[i] <= 0.05f) continue;  // 掩码外不作种子
                    float l = 1.f;
                    if (fr.srcRGBA)
                        l = 0.299f*fr.srcRGBA[i*4+0] + 0.587f*fr.srcRGBA[i*4+1]
                          + 0.114f*fr.srcRGBA[i*4+2];
                    if (l > 0.05f) seed[i] = 1.f;
                }
            }
            // 传播受掩码约束 [修正 2026-08-17]: 原版 Borders — "填充不能穿越透明
            // 区域, 半透明区域限制流动, 完全透明区域视为阻断"。波前在掩码内流动
            // (流体感); 全区域传播+显示裁剪会直线穿过空隙, 观感为"方块扫过"而非
            // "文字的生长" (用户实证)。种子位于掩码外时传播被阻断。
            bfsChebyshev(seed, w, h, dist, pmask, 0.05f);
            if (key) {
                std::lock_guard<std::mutex> lk(g_chebMutex);
                g_chebCache[key] = { (int)n, dist };
                if (g_chebCache.size() > 64) g_chebCache.clear();  // 防膨胀 (≤32 掩码×2 形状)
            }
        }
        float frame = fr.explicitFrames >= 0.f
                    ? fr.explicitFrames
                    : std::max(p01 * fr.totalFrames - 1.f, 0.f);
        // Speed 步进 [2026-08-18]: 原版 Speed = 每秒模拟步数 — 模拟步是离散整数,
        // 帧号取整 → 波前按整步推进; speed < 帧率时出现跳帧 (每 2 帧才推进 1 步)。
        frame = std::floor(frame);
        // 传播缩放: 帧号 → 世界像素 = 帧号 × (maxDist/totalFrames)
        // (渲染世界可能被 AE 放大 (连续栅格化/缩放图层), 传播距离需随世界尺寸自适应;
        //  64×64 图层放大 58 倍 → 30 帧 = 30×58 世界像素, 否则只覆盖一小块 [C])
        // 归一化只统计掩码内像素, 避免透明外扩区域拉大距离范围改变波前速度。
        // 使用 [minD, maxD] 范围而不是 [0, maxD]:
        //   点落在掩码外时, 波前到达内容的距离 minD 可能很大; 若仍按 0..maxD
        //   缩放, 动画会在最后几帧突然全部出现。按内容距离范围映射后,
        //   frame=0 到达最近内容边缘, frame=totalFrames 到达最远内容边缘,
        //   整个时长都在内容上连续演化。
        const float* mask = dmask;
        minD = 1e9f; maxD = 0.f;
        for (size_t i = 0; i < n; i++) {
            if (mask && mask[i] <= 0.05f) continue;
            if (dist[i] >= 1e8f) continue;
            if (dist[i] < minD) minD = dist[i];
            if (dist[i] > maxD) maxD = dist[i];
        }
        if (minD > maxD) { minD = maxD = 0.f; }  // 掩码无有效像素
        float k = std::min(std::max(frame / std::max(fr.totalFrames, 1.f), 0.f), 1.f);
        float framePx = minD + (maxD - minD) * k;
        for (size_t i = 0; i < n; i++) {
            if (dist[i] >= 1e8f) continue;
            // 掩码外显示裁剪 [修正 2026-08-17]: 填充只在掩码内可见 (默认=图层不透明区;
            // 桥接/弱边界时掩码已膨胀 → 溢出区域可见填充) — 与 GPU shader 的
            // outside 标记语义一致 (原版: 掩码外 fill=0)。未裁剪时填充覆盖整个
            // 矩形区域 — 观感为"方块在文字上生长"而非"文字的生长" (用户实证)。
            if (mask && mask[i] <= 0.05f) { baseFill[i] = 0.f; continue; }
            if (dist[i] <= framePx) baseFill[i] = 1.f;
            else if (dist[i] <= framePx + 1.f)
                baseFill[i] = std::max(0.f, 1.f - (dist[i] - framePx));  // 1 帧软边 [B]
        }
        // nd 场 (归一化距离, 0=最近内容 1=最远内容; 掩码外=2.0) — 层 shader 语义
        ndF.resize(n);
        {
            const float spanD = std::max(maxD - minD, 1.f);
            for (size_t i = 0; i < n; i++) {
                if (dist[i] >= 1e8f) { ndF[i] = 2.f; continue; }
                ndF[i] = (dist[i] - minD) / spanD;
            }
        }
    }

    // 生长来源统一 [重构 2026-08-18]: 三种来源的 fillMap 均为 baseFill —
    // 来源差异已在种子构建阶段分派完毕 (原 0x30B80 ramp 直接输出 / luma×baseFill
    // 调制由"噪声白区/源图白区作种子 + BFS 生长"取代, 观感 = 起始点随生长出现并扩散)。
    fillMap = baseFill;

    // ---- 2. Fill_GPU 合成 (0x1425F1) = fill 链 0x16000 的 CPU 9 步 (A 级) ----
    //   链序: 1 warp累加 → 2/3 高斯模糊H/V(0x24F50) → 4 gamma/exposure(0x33080)
    //         → 5 speedOverlay(0x30DC0) → 6-8 清零/拷贝/膨胀(缓冲管线) → 9 borderControl(0x30170)
    //   复刻: 第 1 步的源场已由 splat+BFS 生成 (恒等 quad 等价 [B]);
    //   6-8 步单场 max 累积语义由 dilateMaxField/合成结构覆盖 [B]。
    if (fr.speedMap || fr.edgeMap || fr.blurRadius >= 0.5f) {
        // 2/3 模糊 (param 96)
        if (fr.blurRadius >= 0.5f)
            gaussBlurField24F50(fillMap, w, h, fr.blurRadius);
        // 4 gamma/exposure (0x33080): v>0.0001 ? clamp(pow(v,γ)·exp,0,1) : 0
        if (fr.gammaF != 1.f || fr.exposureF != 1.f) {
            for (size_t i = 0; i < n; i++) {
                float v = fillMap[i];
                if (v < 1.0e-4f) { v = 0.f; }
                else {
                    v = std::pow(v, fr.gammaF) * fr.exposureF;
                    v = std::min(std::max(v, 0.f), 1.f);
                }
                fillMap[i] = v;
            }
        }
        // 5 speedOverlay (0x30DC0): o=overlay(dst,src); out=w·o+(1−w)·dst
        if (fr.speedMap) {
            for (size_t i = 0; i < n; i++)
                fillMap[i] = overlayLerp30E50(fillMap[i], fr.speedMap[i],
                                              fr.speedMapInfluenceF);
        }
        // 9 borderControl (0x30170): dst·(1−w·(1−src)), clamp [0,1]
        if (fr.edgeMap) {
            for (size_t i = 0; i < n; i++) {
                float v = fillMap[i] * (1.f - fr.borderInfluenceF *
                                        (1.f - fr.edgeMap[i]));
                fillMap[i] = std::min(std::max(v, 0.f), 1.f);
            }
        }
        // 注: 不再按 shapeAlpha 二次裁剪 (原版内核无掩码; 波前覆盖整个图层区域)
    }

    // ---- 3. 层着色 (原版 GPU 层 shader 语义, gl_shaders.h 移植自原版 21 GLSL ) ----
    //   权威模型 (shader 逐行):
    //     nd = 归一化距离 (0=最近内容, 1=最远; 形状外=2.0); 波前半径 = p01 直接
    //     层权重 wA = win × fillG × op × colAlpha
    //     win  = smoothstep(lo−WB, lo+WB, p01) × (1 − smoothstep(hi, hi+WB, p01))   WB=0.03
    //     fillG = g==0 ? fill(覆盖率) : 1 − smoothstep(p01, p01+softEdge, nd − g×0.01)
    //             (g≠0 层波前偏移 — 条纹前缘; softEdge = 1px 归一化)
    //     渐变取色 pos = nd (波前位置); gradientMode==2 → (p01−lo)/(hi−lo)
    //     mode==3 原版照画 (源图盖在下层之上 → 文字保持自身颜色, 彩色只填透明背景)
    //   合成顺序 = 预设 "order" 字段 (槽位号, 小者先画=底层; 0x26780 槽位循环顺序)
    std::vector<float> dstR(n, 0.f), dstG(n, 0.f), dstB(n, 0.f), dstA(n, 0.f);
    const float spanD = std::max(maxD - minD, 1.f);
    const float softEdge = 1.f / spanD;          // 1px 波前软边 (归一化空间)
    const float WB = 0.03f;                       // 窗口软边 (shader WB)
    int orderIdx[5];
    for (int li = 0; li < nLayers; li++) orderIdx[li] = li;
    std::stable_sort(orderIdx, orderIdx + nLayers,
        [&](int a, int b) { return pres.layers[a].order < pres.layers[b].order; });
    for (int k = 0; k < nLayers; k++) {
        int li = orderIdx[k];
        if (fr.growthSource == 0 && thresh[li] > p01) continue;  // 点来源: 延迟硬阈值 gating
        const PresetLayer& L = pres.layers[li];
        // 文字模式=填充覆盖: 跳过 mode-3 源图层 (原版它把文字盖在彩色之上)
        if (fr.sourceLayerMode == 1 && L.mode == 3) continue;
        float op = std::min(std::max(L.overlayOpacity, 0.f), 100.f) * 0.01f;
        // 层窗口 (shader 原样): 上沿 lo±WB 淡入, 下沿 hi→hi+WB 淡出 (hi=0.99 时 100% 仍大部分在)
        float lo = std::min(L.start, L.end) * 0.01f;
        float hi = std::max(L.start, L.end) * 0.01f;
        float win = smoothstepField(lo - WB, lo + WB, p01) *
                    (1.f - smoothstepField(hi, hi + WB, p01));
        if (win <= 0.f) continue;
        float g = L.grow;
        bool isGradient = (L.mode == 2 || (L.mode == 0 && L.nStops > 0));
        int nS = std::min(L.nStops, 12);
        for (size_t i = 0; i < n; i++) {
            // 层填充: g≠0 → 波前偏移带 (shader, 需要 nd); g==0 → 覆盖率 (fill 链输出)
            float ndv = ndF[i];
            float fillG;
            if (g != 0.f) {
                if (ndv > 1.f) continue;   // 形状外/无距离 (仅 g 层需要)
                fillG = std::min(std::max(1.f - smoothstepField(p01, p01 + softEdge,
                                                      ndv - g * 0.01f), 0.f), 1.f);
            } else {
                fillG = fillMap[i];
            }
            if (fillG <= 0.f) continue;
            float cr, cg, cb, ca;
            if (L.mode == 1) {
                cr = L.color[0]; cg = L.color[1]; cb = L.color[2]; ca = L.color[3];
            } else if (isGradient && nS > 0) {
                // 渐变取色 = 波前位置 nd (shader: float w = nd);
                //   gradientMode==2 → 窗口内时间位置 (p01−lo)/(hi−lo)
                // 修正 [2026-08-17]: 预设数据中 stops 的 pos 可能乱序 —
                //   按 pos 排序后再查找 (与 GPU 上传前排序一致), 否则取色错段。
                PresetColorStop sorted[12];
                for (int s = 0; s < nS; s++) sorted[s] = L.stops[s];
                for (int a = 0; a < nS; a++)
                    for (int b = a + 1; b < nS; b++)
                        if (sorted[b].pos < sorted[a].pos) std::swap(sorted[a], sorted[b]);
                float pos = ndv;
                if (L.gradientMode == 2) {
                    float span = (hi - lo);
                    pos = span > 0.f ? std::min(std::max((p01 - lo) / span, 0.f), 1.f) : 0.f;
                }
                const PresetColorStop* s0 = &sorted[0];
                const PresetColorStop* s1 = &sorted[0];
                for (int si = 1; si < nS; si++) {
                    if (pos <= sorted[si].pos) { s0 = &sorted[si-1]; s1 = &sorted[si]; break; }
                    if (si == nS - 1) { s0 = &sorted[si]; s1 = &sorted[si]; }
                }
                float span = s1->pos - s0->pos;
                float t = span > 0 ? std::min(std::max((pos - s0->pos) / span, 0.f), 1.f) : 0.f;
                cr = s0->r + (s1->r - s0->r) * t;
                cg = s0->g + (s1->g - s0->g) * t;
                cb = s0->b + (s1->b - s0->b) * t;
                ca = s0->a + (s1->a - s0->a) * t;
            } else if (L.mode == 3) {
                if (fr.srcRGBA) {
                    cr = fr.srcRGBA[i*4+0]; cg = fr.srcRGBA[i*4+1];
                    cb = fr.srcRGBA[i*4+2]; ca = fr.srcRGBA[i*4+3];
                } else { cr = 1.f; cg = 1.f; cb = 1.f; ca = 1.f; }
            } else {
                cr = L.color[0]; cg = L.color[1]; cb = L.color[2]; ca = L.color[3];
            }
            float wA = win * fillG * ca * op;
            // 循环包络 (见 DirectFrame.loopEnv): mode-3 源图层豁免 — 文字始终可见
            if (L.mode != 3) wA *= fr.loopEnv;
            if (wA <= 0.f) continue;
            // 混合模式 (0x3D830 跳表权威映射): 层自带 overlay_mode 优先,
            // 否则用面板混合模式参数; 0/分隔符值回落正常 (src-over)
            int bm = (L.overlayMode != 0) ? L.overlayMode : fr.blendMode;
            blendPixel(&dstR[i], &dstG[i], &dstB[i], &dstA[i],
                       cr, cg, cb, wA, bm, 100.f);
        }
    }

    colorR = dstR; colorG = dstG; colorB = dstB; layerAlpha = dstA;
}

} // namespace dissolve
