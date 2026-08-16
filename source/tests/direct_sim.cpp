/* direct_sim.cpp — 模拟用户场景: 64x64 画布 + 23x7 内容 + preset 27 (Iron Man) */
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
    const int W = 64, H = 64;
    Params p;
    Buffers buf;
    buf.resize(W, H);
    generateNoiseMap(p, buf, W, H);
    // 模拟用户图层: 内容在 (27,48)-(50,55), 其余透明
    std::vector<float> src((size_t)W*H*4, 0.f);
    for (int y = 48; y < 55; y++)
        for (int x = 27; x < 50; x++) {
            size_t i = (size_t)y*W+x;
            src[i*4+0] = 0.5f; src[i*4+1] = 0.5f; src[i*4+2] = 0.5f; src[i*4+3] = 1.f;
        }

    const Preset& pres = kPresets[27];  // Iron Man
    printf("preset=27 %s nLayers=%d duration=%.1f\n", pres.name, pres.nLayers, pres.duration);
    for (int li = 0; li < pres.nLayers; li++) {
        const PresetLayer& L = pres.layers[li];
        printf("  layer %d: mode=%d nStops=%d start=%.1f color=(%.2f,%.2f,%.2f,%.2f)\n",
               li, L.mode, L.nStops, L.start, L.color[0], L.color[1], L.color[2], L.color[3]);
        for (int s = 0; s < std::min(L.nStops, 4); s++)
            printf("    stop %d: (%.2f,%.2f,%.2f,%.2f) pos=%.3f\n", s,
                   L.stops[s].r, L.stops[s].g, L.stops[s].b, L.stops[s].a, L.stops[s].pos);
    }

    const float times[5] = { 0.f, 0.1f, 0.3f, 0.6f, 1.0f };  // 秒
    for (int q = 0; q < 5; q++) {
        float sec = times[q];
        float fps = 30.f;
        float p01 = std::min(std::max((sec * 1.f * fps + 1.f) / std::max(pres.duration * fps, 1.f), 0.f), 1.f);
        DirectFrame fr;
        fr.preset = &pres;
        fr.progress01 = p01;
        fr.explicitFrames = sec * fps;
        fr.splatRadius = 10.f;
        fr.rampS = 1.f;
        fr.srcRGBA = src.data();
        fr.noiseFill = buf.noiseMap.data();
        int nL = pres.nLayers;
        std::vector<float> pts((size_t)nL*2, 0.f), th(nL, 0.f);
        for (int li = 0; li < nL; li++) {
            // 区域中心种子 (renderWorld 当前语义; 设计 {45,45} 图层坐标待换算)
            pts[(size_t)li*2+0] = (float)W * 0.5f;
            pts[(size_t)li*2+1] = (float)H * 0.5f;
            th[li] = pres.layers[li].start / 100.f;
        }
        fr.layerPts = pts.data();
        fr.layerThresh = th.data();
        // 复现 AE renderWorld 完整组装: speedMap (mode1 Luma) + edgeMap
        std::vector<float> spd;
        speedMapFromSource(src.data(), W, H, 1, 0, 1.f, spd);
        fr.speedMap = spd.data();
        fr.speedMapInfluenceF = 0.5f;
        std::vector<float> alphaOnly((size_t)W*H, 0.f);
        for (size_t i = 0; i < (size_t)W*H; i++) alphaOnly[i] = src[i*4+3];
        Buffers buf2;
        buf2.resize(W, H);
        sobelEdges(alphaOnly.data(), buf2, W, H);
        fr.edgeMap = buf2.edgeMap.data();
        fr.borderInfluenceF = 0.f;
        fr.gammaF = 1.f;
        fr.exposureF = 1.f;
        std::vector<float> cR, cG, cB, cA;
        renderPresetDirect(fr, W, H, cR, cG, cB, cA);
        size_t lit = 0;
        float mxA = 0;
        int mxX = 0, mxY = 0;
        for (size_t i = 0; i < (size_t)W*H; i++) {
            if (cA[i] > 0.01f) lit++;
            if (cA[i] > mxA) { mxA = cA[i]; mxX = (int)(i % W); mxY = (int)(i / W); }
        }
        // 内容区域 (27,48)-(50,55) 的填充统计
        size_t litContent = 0;
        for (int y = 48; y < 55; y++)
            for (int x = 27; x < 50; x++)
                if (cA[(size_t)y*W+x] > 0.01f) litContent++;
        printf("t=%.1fs p01=%.3f: 总内容=%zu maxA=%.2f@(%d,%d) 内容区填充=%zu/161\n",
               sec, p01, lit, mxA, mxX, mxY, litContent);
    }
    return 0;
}
