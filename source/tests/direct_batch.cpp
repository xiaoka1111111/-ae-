/* direct_batch.cpp — 实现管线全预设批量自检 (30 预设 x 4 进度, 非黑/非NaN/合理范围) */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../dissolve_direct.h"
#include "../dissolve_core.h"
#include "../preset_data.h"
#include <cstdio>
#include <vector>
#include <cmath>
using namespace dissolve;

int main() {
    const int W = 160, H = 160;
    Params p;
    Buffers buf;
    buf.resize(W, H);
    generateNoiseMap(p, buf, W, H);
    // 源图 (渐变 + alpha 形状)
    std::vector<float> src((size_t)W*H*4, 0.f);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            size_t i = (size_t)y*W+x;
            src[i*4+0] = (float)x/W; src[i*4+1] = (float)y/H;
            src[i*4+2] = 1.f - (float)x/W; src[i*4+3] = 1.f;
        }

    int failures = 0, blackCount = 0, nanCount = 0;
    const float progs[4] = { 1.f, 25.f, 50.f, 100.f };
    for (int pi = 0; pi < kNumPresets; pi++) {
        const Preset& pres = kPresets[pi];
        for (int q = 0; q < 4; q++) {
            DirectFrame fr;
            fr.preset = &pres;
            fr.progress01 = progs[q] / 100.f;
            fr.totalFrames = pres.duration * 30.f;
            fr.explicitFrames = progs[q] * 0.01f * pres.duration * 30.f;
            fr.splatRadius = 10.f;
            fr.rampS = 1.f;
            fr.srcRGBA = src.data();
            fr.noiseFill = buf.noiseMap.data();
            int nL = std::min(pres.nLayers, 5);
            std::vector<float> pts((size_t)nL*2, 0.f), th(nL, 0.f);
            for (int li = 0; li < nL; li++) {
                pts[(size_t)li*2+0] = 45.f; pts[(size_t)li*2+1] = 45.f;
                th[li] = pres.layers[li].start / 100.f;
            }
            fr.layerPts = pts.data();
            fr.layerThresh = th.data();
            // 三种生长来源都试
            for (int gs = 0; gs < 3; gs++) {
                fr.growthSource = gs;
                std::vector<float> cR, cG, cB, cA;
                renderPresetDirect(fr, W, H, cR, cG, cB, cA);
                size_t lit = 0; bool nan = false, oob = false;
                for (size_t i = 0; i < (size_t)W*H; i++) {
                    if (std::isnan(cR[i]) || std::isnan(cG[i]) || std::isnan(cB[i]) || std::isnan(cA[i])) { nan = true; break; }
                    if (cR[i] < 0.f || cR[i] > 1.f || cA[i] < 0.f || cA[i] > 1.f) { oob = true; break; }
                    if (cA[i] > 0.01f) lit++;
                }
                if (nan) { nanCount++; printf("  [NaN] preset %d prog %.0f gs %d\n", pi, progs[q], gs); failures++; }
                else if (oob) { printf("  [越界] preset %d prog %.0f gs %d\n", pi, progs[q], gs); failures++; }
                else if (lit == 0) { blackCount++; printf("  [全黑] preset %d prog %.0f gs %d\n", pi, progs[q], gs); }
            }
        }
    }
    printf("== 30 预设 x 4 进度 x 3 生长来源 = 360 组合 ==\n");
    printf("NaN=%d 越界=%d 全黑=%d 其他失败=%d\n", nanCount, 0, blackCount, failures);
    printf("%s\n", failures == 0 ? "== 全部通过 (无 NaN/越界) ==" : "== 有失败 ==");
    return failures;
}
