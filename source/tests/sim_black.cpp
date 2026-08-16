// sim_black.cpp — 用户"黑底无动画"场景仿真 (预设 0 + 黑内容源 + 大世界区域渲染)
// 实现 renderWorld 的关键路径: copyToFloat → 区域提取 → renderPresetDirect → 最终合成
// 目的: 逐帧打印输出像素, 判定 (a) 动画是否逐帧演化 (b) 输出颜色为何黑
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <vector>
#include "dissolve_direct.h"
#include "dissolve_styles.h"

static float hash01(int x, int y) {  // 确定性假噪声 (非关键)
    unsigned v = (unsigned)(x * 374761393 + y * 668265263) ^ 0x9E3779B9;
    v = (v ^ (v >> 13)) * 1274126177u;
    return (float)((v ^ (v >> 16)) & 0xFFFF) / 65535.f;
}

int main() {
    // 用户场景: 输出世界 400; extent 区域 3744x563 @ (baseX=0, baseY=0) 近似
    const int W = 1000, H = 1000;          // 缩放 1/4 保持比例 (世界)
    const int rw = 936, rh = 141;          // 区域 = 3744/4 x 563/4
    const int baseX = 0, baseY = 0;
    // 源 = 黑内容 (alpha=1) 形状: 内容盒占区域 60% 宽 x 80% 高
    std::vector<float> rgba((size_t)W * H * 4, 0.f);
    for (int y = baseY; y < baseY + rh; y++)
        for (int x = baseX; x < baseX + rw; x++) {
            bool in = (x >= baseX + rw * 20 / 100 && x < baseX + rw * 80 / 100 &&
                       y >= baseY + rh * 10 / 100 && y < baseY + rh * 90 / 100);
            if (in) {
                size_t i = (size_t)y * W + x;
                rgba[i*4+0] = 0.0f; rgba[i*4+1] = 0.0f;
                rgba[i*4+2] = 0.0f; rgba[i*4+3] = 1.0f;   // 黑内容
            }
        }

    // 区域提取 (同 renderWorld)
    std::vector<float> rgbaR((size_t)rw * rh * 4, 0.f), alphaR((size_t)rw * rh, 0.f);
    for (int y = 0; y < rh; y++)
        for (int x = 0; x < rw; x++) {
            size_t di = (size_t)y * rw + x;
            size_t si = (size_t)(y + baseY) * W + (x + baseX);
            rgbaR[di*4+0] = rgba[si*4+0]; rgbaR[di*4+1] = rgba[si*4+1];
            rgbaR[di*4+2] = rgba[si*4+2]; rgbaR[di*4+3] = rgba[si*4+3];
            alphaR[di] = rgba[si*4+3];
        }

    // 速度图/边缘 (近似 renderWorld: 速度图=源 luma 恒 0 → influence 无影响; edge=Sobel 简化为 1)
    std::vector<float> speedMap((size_t)rw * rh, 0.f), edgeMap((size_t)rw * rh, 1.f);

    // 点: 45% 世界 → 区域 (同 renderWorld 换算)
    const int nLayers = 3;  // 预设 0 = 3 层
    std::vector<float> dPts((size_t)nLayers * 2), dTh(nLayers, 0.f);
    for (int li = 0; li < nLayers; li++) {
        float wx = (float)W * 0.45f - (float)baseX;
        float wy = (float)H * 0.45f - (float)baseY;
        float r = 10.f * 0.5f;
        wx = std::min(std::max(wx, r), (float)rw - r);
        wy = std::min(std::max(wy, r), (float)rh - r);
        dPts[(size_t)li*2+0] = wx; dPts[(size_t)li*2+1] = wy;
    }

    const Preset& pres = kPresets[0];  // (Reveal) 2 Color Stripes
    printf("预设: %s (duration=%.2f repeat=%d compOver=%d, %d 层)\n",
           pres.name, pres.duration, pres.repeat, pres.compOverOriginal, pres.nLayers);

    for (int pg = 0; pg <= 100; pg += 10) {
        dissolve::DirectFrame fr;
        fr.preset = &pres;
        fr.progress01 = (float)pg / 100.f;
        float fps = 30.f;
        fr.totalFrames = std::max(pres.duration * fps, 1.f);
        fr.explicitFrames = (float)pg / 100.f * pres.duration * fps;  // 秒×帧率
        fr.splatRadius = 10.f;
        fr.rampS = 1.f; fr.divisor = 1.f;
        fr.growthSource = 0;
        fr.srcRGBA = rgbaR.data();
        fr.shapeAlpha = alphaR.data();
        fr.layerPts = dPts.data(); fr.layerThresh = dTh.data();
        fr.blendMode = 1;
        fr.speedMap = speedMap.data(); fr.speedMapInfluenceF = 0.5f;
        fr.edgeMap = edgeMap.data(); fr.borderInfluenceF = 0.f;
        std::vector<float> cR, cG, cB, cA;
        dissolve::renderPresetDirect(fr, rw, rh, cR, cG, cB, cA);

        // 最终合成 (同 renderWorld compOver)
        size_t lit = 0;
        float probeR = -1, probeG = -1, probeB = -1, probeA = -1;  // 内容中心
        size_t probe = (size_t)(rh / 2) * rw + (rw / 2);
        for (size_t i = 0; i < (size_t)rw * rh; i++) {
            float a = cA[i];
            float srcA = alphaR[i];
            float oR, oG, oB, oA;
            bool compOver = (pres.compOverOriginal == 0);
            if (compOver) {
                oR = rgbaR[i*4+0] * (1 - a) + cR[i] * a;
                oG = rgbaR[i*4+1] * (1 - a) + cG[i] * a;
                oB = rgbaR[i*4+2] * (1 - a) + cB[i] * a;
                oA = std::max(srcA, a);
            } else {
                oR = cR[i] * a + rgbaR[i*4+0] * (1 - a);
                oG = cG[i] * a + rgbaR[i*4+1] * (1 - a);
                oB = cB[i] * a + rgbaR[i*4+2] * (1 - a);
                oA = srcA * (1 - a);
            }
            if (i == probe) { probeR = oR; probeG = oG; probeB = oB; probeA = oA; }
            if (oA > 0.01f) lit++;
        }
        printf("prog=%3d%%  lit=%zu  maxA=%.2f  内容中心输出=(%.2f,%.2f,%.2f,%.2f)\n",
               pg, lit, cA.empty() ? 0.f : *std::max_element(cA.begin(), cA.end()),
               probeR, probeG, probeB, probeA);
    }
    return 0;
}
