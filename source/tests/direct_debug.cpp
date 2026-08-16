/* direct_debug.cpp — 实现管线 Fire 预设诊断 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../dissolve_direct.h"
#include "../dissolve_core.h"
#include "../preset_data.h"
#include <cstdio>
#include <vector>
using namespace dissolve;

int main() {
    const int W = 160, H = 160;
    Params p;
    Buffers buf;
    buf.resize(W, H);
    generateNoiseMap(p, buf, W, H);
    printf("noiseMap 样例: %.3f %.3f %.3f (min/max)\n",
           buf.noiseMap[0], buf.noiseMap[W*H/2], buf.noiseMap[W*H-1]);
    float mn = 1, mx = 0;
    for (size_t i = 0; i < (size_t)W*H; i++) { mn = std::min(mn, buf.noiseMap[i]); mx = std::max(mx, buf.noiseMap[i]); }
    printf("noiseMap min=%.3f max=%.3f\n", mn, mx);

    DirectFrame fr;
    fr.preset = &kPresets[7];  // Fire
    fr.progress01 = 0.5f;
    fr.totalFrames = 60.f;
    fr.splatRadius = 10.f;
    fr.rampS = 1.f;
    fr.noiseFill = buf.noiseMap.data();
    int nL = kPresets[7].nLayers;
    printf("Fire nLayers=%d\n", nL);
    for (int li = 0; li < nL; li++) {
        printf("  layer %d: mode=%d nStops=%d start=%.1f end=%.1f\n",
               li, kPresets[7].layers[li].mode, kPresets[7].layers[li].nStops,
               kPresets[7].layers[li].start, kPresets[7].layers[li].end);
    }
    std::vector<float> pts((size_t)nL*2, 0.f), th(nL, 0.f);
    for (int li = 0; li < nL; li++) {
        pts[(size_t)li*2+0] = W*0.5f; pts[(size_t)li*2+1] = H*0.5f;
        th[li] = kPresets[7].layers[li].start / 100.f;
        printf("  th[%d]=%.3f\n", li, th[li]);
    }
    fr.layerPts = pts.data();
    fr.layerThresh = th.data();
    std::vector<float> cR, cG, cB, cA;
    renderPresetDirect(fr, W, H, cR, cG, cB, cA);
    size_t lit = 0;
    float rS = 0, gS = 0, bS = 0;
    for (size_t i = 0; i < (size_t)W*H; i++)
        if (cA[i] > 0.05f) { lit++; rS += cR[i]; gS += cG[i]; bS += cB[i]; }
    printf("内容像素=%zu 平均色=(%.3f,%.3f,%.3f)\n", lit, lit?rS/lit:0, lit?gS/lit:0, lit?bS/lit:0);
    // 中心像素详情
    size_t ci = (size_t)80*W+80;
    printf("中心: A=%.3f R=%.3f G=%.3f B=%.3f\n", cA[ci], cR[ci], cG[ci], cB[ci]);
    return 0;
}
