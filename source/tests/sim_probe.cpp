// sim_probe.cpp — 可视化自测探针 (用户真实几何: 400 世界, 区域 3744x563@(152,1664),
// 白字内容, 种子 45% 默认/或 97%,55% 用户点位; 预设 0 = 2 Color Stripes)
// 输出: 每个进度一帧 PPM (world 区域裁剪), 供主控 read_image 人工/视觉诊断
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <vector>
#include "dissolve_direct.h"
#include "dissolve_styles.h"

static void writeBMP(const char* fn, int w, int h, const std::vector<float>& rgba) {
    FILE* f = fopen(fn, "wb");
    if (!f) return;
    int rowSize = ((w * 3 + 3) / 4) * 4;
    int dataSize = rowSize * h;
    int fileSize = 54 + dataSize;
    unsigned char hdr[54] = {0};
    hdr[0]='B'; hdr[1]='M';
    hdr[2]=(unsigned char)(fileSize); hdr[3]=(unsigned char)(fileSize>>8);
    hdr[4]=(unsigned char)(fileSize>>16); hdr[5]=(unsigned char)(fileSize>>24);
    hdr[10]=54;
    hdr[14]=40;
    hdr[18]=(unsigned char)(w); hdr[19]=(unsigned char)(w>>8);
    hdr[20]=(unsigned char)(w>>16); hdr[21]=(unsigned char)(w>>24);
    hdr[22]=(unsigned char)(h); hdr[23]=(unsigned char)(h>>8);
    hdr[24]=(unsigned char)(h>>16); hdr[25]=(unsigned char)(h>>24);
    hdr[26]=1; hdr[28]=24;
    hdr[34]=(unsigned char)(dataSize); hdr[35]=(unsigned char)(dataSize>>8);
    hdr[36]=(unsigned char)(dataSize>>16); hdr[37]=(unsigned char)(dataSize>>24);
    fwrite(hdr, 1, 54, f);
    std::vector<unsigned char> row((size_t)rowSize, 0);
    for (int y = h - 1; y >= 0; y--) {   // BMP 底行在前
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            row[(size_t)x*3+0] = (unsigned char)(std::min(std::max(rgba[i*4+2],0.f),1.f)*255.f);  // B
            row[(size_t)x*3+1] = (unsigned char)(std::min(std::max(rgba[i*4+1],0.f),1.f)*255.f);  // G
            row[(size_t)x*3+2] = (unsigned char)(std::min(std::max(rgba[i*4+0],0.f),1.f)*255.f);  // R
        }
        fwrite(row.data(), 1, (size_t)rowSize, f);
    }
    fclose(f);
}

int main(int argc, char** argv) {
    int presetIdx = (argc > 1) ? atoi(argv[1]) : 0;
    int seedMode  = (argc > 2) ? atoi(argv[2]) : 0;   // 0=45%默认 1=用户点位(97.3%,55.5%)
    const int W = 800, H = 800;                        // 1/5 缩放
    const int baseX = 30, baseY = 333;                 // 152/5, 1664/5
    const int rw = 749, rh = 113;                      // 3744/5, 563/5
    // 白字内容: 5 个"字符"白块 (模拟文字, alpha=1) + 少量半透明边缘
    std::vector<float> rgba((size_t)W * H * 4, 0.f);
    auto glyph = [&](int cx, int cw, int top, int bot) {
        for (int y = top; y < bot; y++)
            for (int x = cx; x < cx + cw; x++) {
                size_t i = (size_t)y * W + x;
                rgba[i*4+0] = 1.f; rgba[i*4+1] = 1.f; rgba[i*4+2] = 1.f; rgba[i*4+3] = 1.f;
            }
    };
    // 字符沿区域分布 (x: 60..680, y: 360..410) — 白字
    glyph(60, 70, 360, 410); glyph(200, 70, 360, 410); glyph(340, 70, 360, 410);
    glyph(480, 70, 360, 410); glyph(620, 70, 360, 410);

    std::vector<float> rgbaR((size_t)rw * rh * 4, 0.f), alphaR((size_t)rw * rh, 0.f);
    for (int y = 0; y < rh; y++)
        for (int x = 0; x < rw; x++) {
            size_t di = (size_t)y * rw + x;
            size_t si = (size_t)(y + baseY) * W + (x + baseX);
            rgbaR[di*4+0] = rgba[si*4+0]; rgbaR[di*4+1] = rgba[si*4+1];
            rgbaR[di*4+2] = rgba[si*4+2]; rgbaR[di*4+3] = rgba[si*4+3];
            alphaR[di] = rgba[si*4+3];
        }
    std::vector<float> speedMap((size_t)rw * rh, 0.f), edgeMap((size_t)rw * rh, 1.f);

    const Preset& pres = kPresets[presetIdx];
    printf("预设: %s  种子模式=%d\n", pres.name, seedMode);

    int nLayers = std::min(pres.nLayers, 5);
    std::vector<float> dPts((size_t)nLayers * 2), dTh(nLayers, 0.f);
    for (int li = 0; li < nLayers; li++) {
        float pxPct = 45.f, pyPct = 45.f;
        if (seedMode == 1) { pxPct = 97.3f; pyPct = 55.5f; }  // 用户点位
        float wx = (float)W * (pxPct / 100.f) - (float)baseX;
        float wy = (float)H * (pyPct / 100.f) - (float)baseY;
        float r = 10.f * 0.5f;
        wx = std::min(std::max(wx, r), (float)rw - r);
        wy = std::min(std::max(wy, r), (float)rh - r);
        dPts[(size_t)li*2+0] = wx; dPts[(size_t)li*2+1] = wy;
    }

    char fn[128];
    std::vector<float> prevOut;   // 上一帧输出 (动画强度统计)
    float prevLit = -1.f;
    for (int pg = 0; pg <= 100; pg += 10) {
        dissolve::DirectFrame fr;
        fr.preset = &pres;
        fr.progress01 = (float)pg / 100.f;
        fr.totalFrames = 30.f;
        fr.explicitFrames = (float)pg / 100.f * 30.f;
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
        // 最终合成 (compOver)
        bool compOver = (pres.compOverOriginal == 0);
        std::vector<float> out((size_t)rw * rh * 4, 0.f);
        size_t lit = 0;
        for (size_t i = 0; i < (size_t)rw * rh; i++) {
            float a = cA[i], srcA = alphaR[i];
            float oR, oG, oB, oA;
            if (compOver) {
                oR = rgbaR[i*4+0]*(1-a) + cR[i]*a;
                oG = rgbaR[i*4+1]*(1-a) + cG[i]*a;
                oB = rgbaR[i*4+2]*(1-a) + cB[i]*a;
                oA = std::max(srcA, a);
            } else {
                oR = cR[i]*a + rgbaR[i*4+0]*(1-a);
                oG = cG[i]*a + rgbaR[i*4+1]*(1-a);
                oB = cB[i]*a + rgbaR[i*4+2]*(1-a);
                oA = srcA*(1-a);
            }
            out[i*4+0]=oR; out[i*4+1]=oG; out[i*4+2]=oB; out[i*4+3]=oA;
            if (a > 0.01f) lit++;
        }
        snprintf(fn, sizeof(fn), "probe_p%02d_s%d.bmp", presetIdx, pg);
        writeBMP(fn, rw, rh, out);
        // 自检指标: 视觉变化率 (与上一帧相比, RGB 变化 ≥0.05 的像素占比) + 颜色统计
        size_t changed = 0, bluePx = 0, orangePx = 0, whitePx = 0;
        if (!prevOut.empty()) {
            for (size_t i = 0; i < (size_t)rw * rh; i++) {
                float d = std::fabs(out[i*4+0]-prevOut[i*4+0]) +
                          std::fabs(out[i*4+1]-prevOut[i*4+1]) +
                          std::fabs(out[i*4+2]-prevOut[i*4+2]);
                if (d > 0.05f) changed++;
            }
        }
        for (size_t i = 0; i < (size_t)rw * rh; i++) {
            float r = out[i*4+0], g = out[i*4+1], b = out[i*4+2], a = out[i*4+3];
            if (a < 0.01f) continue;
            if (b > 0.3f && b > r*1.5f && g > 0.1f && g < 0.35f) bluePx++;
            else if (r > 0.85f && g > 0.55f && g < 0.9f && b < 0.15f) orangePx++;
            else if (r > 0.9f && g > 0.9f && b > 0.9f) whitePx++;
        }
        float chgPct = (float)changed / (float)((size_t)rw * rh) * 100.f;
        printf("prog=%3d%% lit=%zu 变化率=%.1f%%/步  蓝=%zu 橙=%zu 白=%zu  帧=%s\n",
               pg, lit, chgPct, bluePx, orangePx, whitePx, fn);
        prevOut = out;
    }
    return 0;
}
