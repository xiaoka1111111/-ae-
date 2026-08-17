/*
 * dissolve_styles.cpp — 预设多层波次渲染实现
 *
 * 核心语义:
 *   - 种子: 圆点笔刷, 波前从点向外传播
 *   - fillMap: 覆盖率场 (0=未填充, 1=已填充, 波前边缘平滑)
 *   - 层 start/end: fillMap 覆盖率区间 [start/100, end/100] 带通
 *   - gradient_mode 1: 渐变沿波前位置取色 (径向); 2: 按层内时间取色
 *   - displace: 噪声坐标置换 (扭曲波前/采样位置)
 */
#include "dissolve_styles.h"
#include <algorithm>
#include <cmath>
#include <queue>

namespace dissolve {

float progressFromTime(const Preset& p, float seconds) {
    // duration = 完整动画周期秒数 (repeat 次重复, 每次 cycle = dur/repeat)
    // 播放 1 个 cycle 完成一次生长 (0-100%)
    float dur = std::max(p.duration, 0.01f);
    float cycle = dur / std::max(p.repeat, 1);
    float t = std::fmod(std::max(seconds, 0.f), cycle) / cycle;  // 0-1
    return t * 100.f;  // 0-100
}

void sampleGradient(const PresetColorStop* stops, int n, float pos,
                    float* r, float* g, float* b, float* a) {
    if (n <= 0 || !stops) { *r = *g = *b = 1.f; *a = 1.f; return; }
    n = std::min(n, 12);  // 防御: 与数组容量一致
    pos = std::min(std::max(pos, 0.f), 1.f);
    if (n == 1 || pos <= stops[0].pos) {
        *r = stops[0].r; *g = stops[0].g; *b = stops[0].b; *a = stops[0].a;
        return;
    }
    for (int i = 1; i < n; i++) {
        if (pos <= stops[i].pos) {
            const auto& s0 = stops[i-1];
            const auto& s1 = stops[i];
            float span = s1.pos - s0.pos;
            float f = span > 0 ? (pos - s0.pos) / span : 0.f;
            *r = s0.r + (s1.r - s0.r) * f;
            *g = s0.g + (s1.g - s0.g) * f;
            *b = s0.b + (s1.b - s0.b) * f;
            *a = s0.a + (s1.a - s0.a) * f;
            return;
        }
    }
    const auto& s = stops[n-1];
    *r = s.r; *g = s.g; *b = s.b; *a = s.a;
}

// 亮度/饱和度辅助 (PF_Xfer_COLOR: 源色相饱和 + 目标亮度)
static inline float lumaOf(float r, float g, float b) {
    return 0.2126f*r + 0.7152f*g + 0.0722f*b;
}
static inline void setLum(float* r, float* g, float* b, float lum) {
    float d = lum - lumaOf(*r, *g, *b);
    *r += d; *g += d; *b += d;
    float mx = std::max(*r, std::max(*g, *b));
    if (mx > 1.f) { float k = 1.f/mx; *r *= k; *g *= k; *b *= k; }
    float mn = std::min(*r, std::min(*g, *b));
    if (mn < 0.f) { float l = lumaOf(*r,*g,*b); float k = l/(l-mn);
        *r = l + (*r-l)*k; *g = l + (*g-l)*k; *b = l + (*b-l)*k; }
}
static inline float satOf(float r, float g, float b) {
    return std::max(r, std::max(g, b)) - std::min(r, std::min(g, b));
}
static inline void setSat(float* r, float* g, float* b, float s) {
    // 按亮度排序保持色相, 饱和缩放到 s
    float* mnP = r, *mdP = g, *mxP = b;
    if (*mnP > *mdP) std::swap(mnP, mdP);
    if (*mdP > *mxP) std::swap(mdP, mxP);
    if (*mnP > *mdP) std::swap(mnP, mdP);
    if (s > 0.f) {
        if (*mxP > *mnP) {
            *mdP = (*mdP - *mnP) * s / (*mxP - *mnP);
            *mxP = s;
        } else { *mdP = *mxP = 0.f; }
        *mnP = 0.f;
    } else { *mnP = *mdP = *mxP = 0.f; }
}

void blendPixel(float* dr, float* dg, float* db, float* da,
                float sr, float sg, float sb, float sa,
                int mode, float opacity) {
    float o = std::min(std::max(opacity, 0.f), 100.f) * 0.01f;
    if (o <= 0.f || sa <= 0.f) return;
    // src-over 语义: 透明黑底 (alpha=0 且颜色=0) + 任何源 = 源
    // (仅当 dst 完全透明黑时跳过混合公式, 防止 Multiply/ColorBurn 在透明底算出纯黑)
    if (*da <= 0.001f && *dr <= 0.001f && *dg <= 0.001f && *db <= 0.001f) {
        *dr = sr; *dg = sg; *db = sb; *da = sa * o;
        return;
    }
    // 模版/剪影族: 只改目标 alpha, RGB 仍按 src-over 前向合成 (PF_Xfer 语义)
    if (mode == BM_STENCIL_ALPHA || mode == BM_STENCIL_LUMA ||
        mode == BM_SILHOU_ALPHA || mode == BM_SILHOU_LUMA) {
        float k = sa * o;
        if (mode == BM_STENCIL_ALPHA)      { /* dstA *= srcA */ }
        else if (mode == BM_STENCIL_LUMA)  { k *= lumaOf(sr, sg, sb); }
        else if (mode == BM_SILHOU_ALPHA)  { k = 1.f - k; }
        else                               { k = 1.f - k * lumaOf(sr, sg, sb); }
        // RGB: 源前向合成; alpha: 相乘
        *dr = *dr + (sr - *dr) * (sa * o);
        *dg = *dg + (sg - *dg) * (sa * o);
        *db = *db + (sb - *db) * (sa * o);
        *da = std::min(std::max(*da * k, 0.f), 1.f);
        return;
    }
    float br, bg, bb;
    switch (mode) {
    case BM_MULTIPLY:
        br = *dr*sr; bg = *dg*sg; bb = *db*sb; break;
    case BM_COLOR_BURN:  // 经典颜色加深: 1-(1-B)/S (S=0 时保持 B)
        br = sr > 0.f ? 1.f - std::min((1.f - *dr) / sr, 1.f) : 0.f;
        bg = sg > 0.f ? 1.f - std::min((1.f - *dg) / sg, 1.f) : 0.f;
        bb = sb > 0.f ? 1.f - std::min((1.f - *db) / sb, 1.f) : 0.f;
        break;
    case BM_ADD:
        br = *dr + sr; bg = *dg + sg; bb = *db + sb; break;
    case BM_SCREEN:
        br = 1.f - (1.f - *dr) * (1.f - sr);
        bg = 1.f - (1.f - *dg) * (1.f - sg);
        bb = 1.f - (1.f - *db) * (1.f - sb);
        break;
    case BM_OVERLAY:
        br = *dr < 0.5f ? 2*(*dr)*sr : 1 - 2*(1-*dr)*(1-sr);
        bg = *dg < 0.5f ? 2*(*dg)*sg : 1 - 2*(1-*dg)*(1-sg);
        bb = *db < 0.5f ? 2*(*db)*sb : 1 - 2*(1-*db)*(1-sb); break;
    case BM_SOFT_LIGHT: {
        // W3C 标准柔光: B<=0.25 -> ((16B-12)B+4)B; 否则 sqrt(B)
        auto g = [](float B) {
            return B <= 0.25f ? ((16.f*B - 12.f)*B + 4.f)*B : std::sqrt(B);
        };
        auto f = [&](float B, float S) {
            return S <= 0.5f ? B - (1.f - 2.f*S)*B*(1.f - B)
                             : B + (2.f*S - 1.f)*(g(B) - B);
        };
        br = f(*dr, sr); bg = f(*dg, sg); bb = f(*db, sb);
        break; }
    case BM_COLOR: {  // 源色相/饱和 + 目标亮度
        float rr = sr, gg = sg, bb2 = sb;
        setSat(&rr, &gg, &bb2, satOf(*dr, *dg, *db));
        setLum(&rr, &gg, &bb2, lumaOf(*dr, *dg, *db));
        br = rr; bg = gg; bb = bb2;
        break; }
    case BM_NORMAL:
    default:            br = sr; bg = sg; bb = sb; break;
    }
    br = std::min(std::max(br, 0.f), 1.f);
    bg = std::min(std::max(bg, 0.f), 1.f);
    bb = std::min(std::max(bb, 0.f), 1.f);
    float outA = sa * o;
    // alpha 合成 (src over)
    *dr = *dr + (br - *dr) * outA;
    *dg = *dg + (bg - *dg) * outA;
    *db = *db + (bb - *db) * outA;
    *da = *da + outA * (1.f - *da);
}

// 高斯模糊 (旧 styles 路径用, 抽头稀疏化近似 — 仅 tests/renderPreset 死代码路径调用;
// AE 实际路径用 dissolve_direct.cpp 的 gaussBlurField24F50 全抽头版 (0x24F50 , A 级)):
//   权重 exp(-0.5*(x/rL)^2/0.06) (sigma=0.3), 归一化;
//   浮点半径: r1=floor(radius), r2=r1+1, 结果按 fract(radius) 插值
//   边界: clamp (与原版纹理 clamp-to-edge 一致)
static void gaussBlurField(std::vector<float>& field, int w, int h, float radius) {
    if (radius < 0.5f) return;
    int r1 = (int)std::floor(radius);
    int r2 = r1 + 1;
    float alpha = radius - (float)r1;  // fract(radius)
    const size_t n = (size_t)w * h;
    std::vector<float> h1(n), h2(n), v1(n), v2(n);

    auto blur1d = [&](std::vector<float>& dst, const std::vector<float>& src,
                      bool horiz, int rL) {
        if (rL <= 0) { dst = src; return; }
        // 性能: 大半径时步长稀疏采样 (tap 上限 ~17), 保持高斯权重形状 (sigma=0.3 相对)
        int step = std::max(1, (rL + 8) / 16);
        const float invR = 1.0f / (float)rL;
        // 权重预计算 (与像素无关, 每半径只算一次 exp)
        int ntap = 0;
        for (int i = -rL; i <= rL; i += step) ntap++;
        std::vector<float> wgt(ntap);
        std::vector<int> off(ntap);
        {
            int k = 0;
            for (int i = -rL; i <= rL; i += step, k++) {
                float xi = (float)i * invR;
                wgt[k] = std::exp(-0.5f * (xi * xi) / 0.06f);
                off[k] = i;
            }
        }
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                float acc = 0.f, tw = 0.f;
                for (int k = 0; k < ntap; k++) {
                    int px = horiz ? x + off[k] : x;
                    int py = horiz ? y : y + off[k];
                    // clamp 到边缘 (纹理 clamp-to-edge)
                    if (px < 0) px = 0; if (px >= w) px = w - 1;
                    if (py < 0) py = 0; if (py >= h) py = h - 1;
                    acc += wgt[k] * src[(size_t)py * w + px];
                    tw += wgt[k];
                }
                dst[(size_t)y * w + x] = tw > 0.f ? acc / tw : 0.f;
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

// 自动选点: 形状内部距边缘最远点 (distField 最大 = 最大内切圆圆心)
static void autoSelectSeed(const Buffers& buf, int w, int h, int* sx, int* sy) {
    *sx = w / 2; *sy = h / 2;
    float best = -1.f;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float d = buf.distField[(size_t)y*w+x];
            if (d > best) { best = d; *sx = x; *sy = y; }
        }
    }
}

// 圆点笔刷种子掩码 (原版 shader 0x140E96 ):
//   circle(pt): uv = fragCoord - pt; sq_len = |uv|^2
//   factor = 1 if sq_len <= sq_rad; (sq_rad-sq_len)/(sq_rad-sq_margin) if in (margin, rad]
//   sq_margin = sq_rad + sq_fade - 2*fade*radiusF, fade=1
//   多点多源取 max; aspectF/smallestAxisF 在原版中声明未用 (纯像素圆)
void brushSeedMask(const float* pts, int n, float radiusF, int w, int h,
                   std::vector<float>& mask) {
    mask.assign((size_t)w * h, 0.f);
    if (!pts || n <= 0 || radiusF <= 0.f) return;
    n = std::min(n, 5);
    const float fade = 1.0f;
    const float sq_rad = radiusF * radiusF;
    const float sq_margin = sq_rad + fade * fade - 2.f * fade * radiusF;
    const float denom = (sq_rad - sq_margin) > 1e-6f ? (sq_rad - sq_margin) : 1e-6f;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float best = 0.f;
            for (int k = 0; k < n; k++) {
                float dx = (float)x - pts[k * 2 + 0];
                float dy = (float)y - pts[k * 2 + 1];
                float sq_len = dx * dx + dy * dy;
                float factor = 0.f;
                if (sq_len <= sq_rad) {
                    factor = 1.f;
                    if (sq_len > sq_margin)
                        factor = (sq_rad - sq_len) / denom;
                }
                if (factor > best) best = factor;
            }
            mask[(size_t)y * w + x] = best;
        }
    }
}

// BFS 多源距离场 (从种子点向外, 8 邻域, 与原版 dilate 3x3 扩张一致)
// 只在形状内 (alpha > threshold) 传播; 形状外保持极大值 (永不填充)
static void bfsFromMask(const float* mask, int w, int h,
                        const float* srcRGBA, float alphaThresh,
                        std::vector<float>& distOut, float* maxDistOut) {
    distOut.assign((size_t)w * h, 1e9f);
    std::queue<int> qx, qy;
    float maxD = 0.f;
    static const int dx[8] = {1,-1,0,0,1,1,-1,-1};
    static const int dy[8] = {0,0,1,-1,1,-1,1,-1};
    // 种子初始化: 掩码>0.05 且形状内
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            if (mask[i] <= 0.05f) continue;
            if (srcRGBA && srcRGBA[i * 4 + 3] <= alphaThresh) continue;
            distOut[i] = 0.f; qx.push(x); qy.push(y);
        }
    }
    if (qx.empty()) { *maxDistOut = 1.f; return; }
    while (!qx.empty()) {
        int x = qx.front(); qx.pop();
        int y = qy.front(); qy.pop();
        float d = distOut[(size_t)y * w + x];
        if (d > maxD) maxD = d;
        for (int k = 0; k < 8; k++) {
            int nx = x + dx[k], ny = y + dy[k];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            size_t ni = (size_t)ny * w + nx;
            if (distOut[ni] > d + 1.f) {
                if (srcRGBA && srcRGBA[ni * 4 + 3] <= alphaThresh) continue;
                distOut[ni] = d + 1.f; qx.push(nx); qy.push(ny);
            }
        }
    }
    *maxDistOut = maxD;
}

// BFS 单源距离场 (从种子点向外, 8 邻域, 与原版 dilate 3x3 扩张一致)
static void bfsFromSeed(int sx, int sy, int w, int h,
                        const float* srcRGBA, float alphaThresh,
                        std::vector<float>& distOut, float* maxDistOut) {
    distOut.assign((size_t)w*h, 1e9f);
    std::queue<int> qx, qy;
    size_t si = (size_t)sy*w+sx;
    // 种子必须在形状内, 否则整体不填充
    bool seedInside = srcRGBA ? (srcRGBA[si*4+3] > alphaThresh) : true;
    if (!seedInside) { *maxDistOut = 1.f; return; }
    distOut[si] = 0.f;
    qx.push(sx); qy.push(sy);
    static const int dx[8] = {1,-1,0,0,1,1,-1,-1};
    static const int dy[8] = {0,0,1,-1,1,-1,1,-1};
    float maxD = 0.f;
    while (!qx.empty()) {
        int x = qx.front(); qx.pop();
        int y = qy.front(); qy.pop();
        float d = distOut[(size_t)y*w+x];
        if (d > maxD) maxD = d;
        for (int k = 0; k < 8; k++) {
            int nx = x+dx[k], ny = y+dy[k];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            size_t ni = (size_t)ny*w+nx;
            if (distOut[ni] > d + 1.f) {
                if (srcRGBA && srcRGBA[ni*4+3] <= alphaThresh) continue;  // 形状外不传播
                distOut[ni] = d + 1.f; qx.push(nx); qy.push(ny);
            }
        }
    }
    *maxDistOut = maxD;
}

void renderPreset(const StyleFrame& fr,
                  const Buffers& buf, const float* srcRGBA, int w, int h,
                  std::vector<float>& colorR, std::vector<float>& colorG,
                  std::vector<float>& colorB, std::vector<float>& layerAlpha) {
    size_t n = (size_t)w * h;
    colorR.assign(n, 0.f); colorG.assign(n, 0.f); colorB.assign(n, 0.f);
    layerAlpha.assign(n, 0.f);
    if (!fr.preset) return;

    // ---- 波前传播: 从种子点向外, 播放进度 = 传播帧号 ----
    // 原版: Growth 每帧传播 1px (timeF 累加), time1 记录到达帧号;
    // 层窗口 start/end = 传播帧号区间 (0-99): 波前扫过时各层按时间依次接管
    // (Looped Gradients 0-22/20-42/40-62/60-82/80-99 循环; 2 Color Stripes 橙带 30-60)
    int sx, sy;
    std::vector<float> bfsDist;
    float maxDist = 1.f;
    if (fr.seedMask) {
        // 圆点笔刷多源传播 (原版用户放置点)
        bfsFromMask(fr.seedMask, w, h, srcRGBA, 0.05f, bfsDist, &maxDist);
        // 退化: 无有效种子 (点全部在形状外) -> 自动选点 (形状最深点)
        bool anySeed = false;
        for (size_t i = 0; i < n; i++) if (bfsDist[i] < 1e8f) { anySeed = true; break; }
        if (!anySeed) {
            autoSelectSeed(buf, w, h, &sx, &sy);
            bfsFromSeed(sx, sy, w, h, srcRGBA, 0.05f, bfsDist, &maxDist);
        }
    } else {
        autoSelectSeed(buf, w, h, &sx, &sy);
        bfsFromSeed(sx, sy, w, h, srcRGBA, 0.05f, bfsDist, &maxDist);
    }
    if (maxDist < 1e-4f) maxDist = 1.f;

    float prog = std::min(std::max(fr.progress, 0.f), 100.f);
    float p01 = prog / 100.f;  // 归一化传播进度 (0-1)
    // 波前软边 = 1 像素 (原版 coverage 为 8x8 子像素采样, 软边 <=1px; 0x142C71)
    const float SOFT = 1.f / std::max(maxDist, 1.f);

    // fillMap: 传播覆盖率场 (已填充=1, 未填充=0, 波前边缘平滑)
    // nd 归一化到 [0,1] (1=最远像素); 波前推进 p01 时 nd<=p01 已填充
    // p01=1.0 时全部填充 (smoothstep 上沿对齐到 1.0 之外)
    // 关键: 形状外 (bfsDist=1e9, nd 会被钳到 1) 必须强制 0 —
    //       否则进度 100% 时形状外也被填充 (层色溢出边界铺满透明区)
    std::vector<float> fillMap(n);
    for (size_t i = 0; i < n; i++) {
        if (bfsDist[i] >= 1e8f) { fillMap[i] = 0.f; continue; }
        float nd = std::min(bfsDist[i] / maxDist, 1.f);  // 0=种子, 1=最远
        // 已填充: nd < p01; 波前边缘平滑 SOFT
        // 用 1-smoothstep(p01, p01+SOFT, nd): nd<=p01 -> 1, nd>=p01+SOFT -> 0
        float f = 1.f - smoothstepField(p01, p01 + SOFT, nd);
        fillMap[i] = std::min(std::max(f, 0.f), 1.f);
    }

    // ---- Fill_GPU 合成 (原版: fillMap 经 speedOverlay + borderControl 再进层渲染) ----
    // speed = 源图像素速度图 (原版 0x332C0 内核 + 面板 Channel 参数);
    // border = Sobel 边缘; 只有调用方提供 params 时应用 (nullptr = 跳过, 兼容旧调用)
    std::vector<float> fillAdj;
    if (fr.params) {
        // 全局模糊半径 (面板 Fill > Blur Radius): 对 fillMap 高斯模糊 (原版 0x141C21/0x142071)
        std::vector<float> fillMapBlurred;
        const float* fillIn = fillMap.data();
        if (fr.params->blurRadius > 0.5f) {
            fillMapBlurred = fillMap;
            gaussBlurField(fillMapBlurred, w, h, fr.params->blurRadius);
            fillIn = fillMapBlurred.data();
        }
        fillAdj.assign(n, 0.f);
        Buffers tmp;
        tmp.w = w; tmp.h = h;
        // 速度图: srcRGBA 可用时按面板 Channel 从源图像素生成 (原版 0x332C0);
        // 否则缺省 = 距离场
        if (srcRGBA) {
            if (fr.params->speedMapMode <= 0) {
                // 面板速度图模式 = "无": 速度图全 0 (Fill_GPU overlay(v0,0) 无意义,
                // 调用方应同时把 speedMapInfluenceF 置 0; 双保险置全 0)
                tmp.speedMap.assign((size_t)w * h, 0.f);
            } else {
                speedMapFromSource(srcRGBA, w, h,
                                   std::max(fr.params->speedMapMode, 1),
                                   fr.params->speedMapChannel,
                                   std::max(fr.params->speedMapScale, 0.1f),
                                   tmp.speedMap);
            }
        } else {
            tmp.speedMap = (buf.speedMap.size() == n) ? buf.speedMap : buf.distField;
        }
        tmp.edgeMap  = (buf.edgeMap.size() == n)  ? buf.edgeMap
                                                  : std::vector<float>(n, 0.f);  // 边框图缺省 = 0
        // 边框扩展 (面板 Borders > Border Expand): edgeMap 3x3 迭代膨胀
        if (fr.params->borderExpand > 0 && tmp.edgeMap.size() == n) {
            for (int iter = 0; iter < fr.params->borderExpand; iter++) {
                std::vector<float> dil(n, 0.f);
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++) {
                        float mx = 0.f;
                        for (int dy = -1; dy <= 1; dy++)
                            for (int dx = -1; dx <= 1; dx++) {
                                int px = x+dx, py = y+dy;
                                if (px < 0 || py < 0 || px >= w || py >= h) continue;
                                float v = tmp.edgeMap[(size_t)py*w+px];
                                if (v > mx) mx = v;
                            }
                        dil[(size_t)y*w+x] = mx;
                    }
                tmp.edgeMap.swap(dil);
            }
        }
        fillComposite(*fr.params, tmp, fillIn, w, h, fillAdj.data());
    }
    const std::vector<float>& fillSrc = fr.params ? fillAdj : fillMap;

    std::vector<float> orderR(n, 0.f), orderG(n, 0.f), orderB(n, 0.f), orderA(n, 0.f);
    std::vector<float> layerR, layerG, layerB, layerA;

    const Preset& p = *fr.preset;
    int nLayers = std::min(p.nLayers, 5);  // 防御: layers[5] 容量
    if (nLayers <= 0) return;

    // 渲染顺序 = 预设数据顺序 (原版 0x18060 层循环 0x18BC3 按记录数组遍历, 无 order 排序;
    // 后画的层覆盖先画的层 — order 字段不参与排序)
    int idx[5];
    for (int i = 0; i < nLayers; i++) idx[i] = i;

    for (int li = 0; li < nLayers; li++) {
        const PresetLayer& L0 = p.layers[idx[li]];
        // 每层拷贝 stops 并按 pos 排序 (原版数据 pos 乱序, 采样需升序)
        PresetColorStop sorted[12];
        int nS = std::min(L0.nStops, 12);
        for (int s = 0; s < nS; s++) sorted[s] = L0.stops[s];
        for (int a = 0; a < nS; a++)
            for (int b = a+1; b < nS; b++)
                if (sorted[b].pos < sorted[a].pos) std::swap(sorted[a], sorted[b]);
        const PresetLayer& L = L0;

        // 层窗口: 传播进度区间 [start/100, end/100] (原版: 波前到达帧号区间)
        // 层在波前推进到 start% 时开始显现, end% 时淡出 — 播放动画
        // 上沿: 从 hi 开始淡出 (hi=99 时 100% 完全打开, 不会被软边减半)
        float lo = std::min(L.start, L.end) / 100.f;
        float hi = std::max(L.start, L.end) / 100.f;
        const float WB = 0.03f;  // 窗口软边
        float win = smoothstepField(lo - WB, lo + WB, p01) *
                    (1.f - smoothstepField(hi, hi + WB, p01));

        layerR.assign(n, 0.f); layerG.assign(n, 0.f); layerB.assign(n, 0.f);
        layerA.assign(n, 0.f);

        // 层填充场: fillSrc + grow 线性调制 (原版无 pow; 形态学膨胀/线性缩放证据 0x369D0)
        //   grow>0 = 波前提前 (膨胀方向), grow<0 = 延后; 系数 0.01 为推断 [C]
        std::vector<float> layerFill(n);
        if (L.grow != 0.f) {
            for (size_t i = 0; i < n; i++) {
                if (fillSrc[i] <= 0.001f) { layerFill[i] = 0.f; continue; }
                float nd = std::min(bfsDist[i] / maxDist, 1.f);
                float ndAdj = std::min(std::max(nd - L.grow * 0.01f, 0.f), 1.f);
                float fAdj = 1.f - smoothstepField(p01, p01 + SOFT, ndAdj);
                layerFill[i] = std::min(std::max(fAdj, 0.f), 1.f);
            }
        } else {
            layerFill = fillSrc;
        }
        // blur: 对填充场高斯模糊 (原版 0x141C21/0x142071 输入=fillMap 单通道;
        // 模糊自然外扩到未填充区 = 柔光; 仅形状外保持 0)
        if (L.blur > 0.5f) {
            gaussBlurField(layerFill, w, h, L.blur * 0.1f);
            if (srcRGBA) {
                for (size_t i = 0; i < n; i++)
                    if (srcRGBA[i*4+3] <= 0.05f) layerFill[i] = 0.f;
            }
        }

        for (size_t i = 0; i < n; i++) {
            int px = (int)(i % w), py = (int)(i / w);
            // displace: 采样坐标置换 (原版 0x332C0: dst = 四边形插值 - [C+0x68/0x6c] 位移;
            // Noise_X/Y_Displacement 双组件 — 噪声位移后坐标采样距离场/填充场, 扭曲填充与取色)
            float ndEff = bfsDist[i] / maxDist;
            int sx2 = px, sy2 = py;
            if (L.displaceSize > 0.f && L.displace > 0.f) {
                float nv = buf.noiseMap[i];
                // 单通道噪声近似 X/Y 双位移 (原版双组件 [C]; 缩放系数为推断)
                float off = (nv - 0.5f) * 2.f * L.displace * 0.01f * (L.displaceSize * 0.01f);
                sx2 = std::min(std::max(px + (int)(off * (float)w), 0), w - 1);
                sy2 = std::min(std::max(py + (int)(off * (float)h), 0), h - 1);
                ndEff = std::min(bfsDist[(size_t)sy2 * w + sx2] / maxDist, 1.f);
            }
            // 层激活 = 时间窗口 × 填充场 (blur 后柔光外扩; 扭曲采样 = 原版坐标置换)
            float wA = win * layerFill[(size_t)sy2 * w + sx2];
            if (wA <= 0.001f) continue;

            // 渐变取色位置 (displace 后扭曲位置; grow/blur 不影响取色)
            float wave = ndEff;

            // 颜色
            // mode: 1=实色 2=渐变 3=原图显现; 0=原版无 mode 字段(缺省渐变)
            float cr, cg, cb, ca;
            if (L.mode == 1) {  // 实色
                cr = L.color[0]; cg = L.color[1]; cb = L.color[2]; ca = L.color[3];
            } else if (L.mode == 2 || (L.mode == 0 && L.nStops > 0)) {  // 渐变
                // 实证 (速度图参数.md §2): 原版渐变描述符 stop 位置 =
                //   ratio·param(0x16/0x17)/(divisor·rec_w/h) = 归一化层坐标 [A];
                //   gradient_mode CPU 侧语义 = divisor 表 {1,1,2,0.5} 索引 (缩放 stop 位置),
                //   不是取色方向 — 原"1=沿波前/2=层内进度"分支无证据 [C]
                // 取色 pos 来源: 原版 = 归一化层坐标 (param 0x16/0x17 层内坐标, 复刻无此
                //   参数); 渐变颜色→填充映射函数在 GPU Fill 路径, CPU 侧未发现 [C]
                //   复刻暂以 wave 波前距离近似 pos; divisor 缩放 stop 位置待 AE 实测
                //   对照后接入 (改动会改变全部渐变预设视觉, 需实测验证)
                float pos;
                if (L.gradientMode == 2) {
                    float span = (hi - lo);
                    pos = span > 0 ? std::min(std::max((p01 - lo) / span, 0.f), 1.f) : 0.f;
                } else {
                    pos = wave;
                }
                if (nS > 0) sampleGradient(sorted, nS, pos, &cr, &cg, &cb, &ca);
                else { cr = L.color[0]; cg = L.color[1]; cb = L.color[2]; ca = L.color[3]; }
            } else if (L.mode == 3) {  // 原图显现层 (波前到达后显示源图)
                if (srcRGBA) {
                    cr = srcRGBA[i*4+0]; cg = srcRGBA[i*4+1];
                    cb = srcRGBA[i*4+2]; ca = srcRGBA[i*4+3];
                } else {
                    cr = 1.f; cg = 1.f; cb = 1.f; ca = 1.f;
                }
            } else {  // mode=0 无 stops: 退化用实色
                cr = L.color[0]; cg = L.color[1]; cb = L.color[2]; ca = L.color[3];
            }
            layerR[i] = cr; layerG[i] = cg; layerB[i] = cb;
            // 关键: 渐变/实色的颜色 alpha 参与层权重 (透明色不覆盖原图)
            layerA[i] = wA * ca;
        }

        // overlay_opacity
        float op = L.overlayOpacity;

        // 面板"混合模式"覆盖层 blendMode (0x3D830 跳表权威映射, 1-based 值):
        //   层自带 overlayMode != 0 时优先 (预设数据); 否则用面板参数 (默认 1=正常)
        int bm = (L.overlayMode != 0) ? L.overlayMode
                                      : (fr.params ? fr.params->blendMode : 1);
        if (bm <= 0) bm = 1;

        // 合成到 order 缓冲 (order 小者先画=底层, 大者后画=顶层)
        for (size_t i = 0; i < n; i++) {
            blendPixel(&orderR[i], &orderG[i], &orderB[i], &orderA[i],
                       layerR[i], layerG[i], layerB[i], layerA[i],
                       bm, op);
        }
    }

    // 输出
    for (size_t i = 0; i < n; i++) {
        colorR[i] = orderR[i]; colorG[i] = orderG[i]; colorB[i] = orderB[i];
        layerAlpha[i] = orderA[i];
    }
}

} // namespace dissolve
