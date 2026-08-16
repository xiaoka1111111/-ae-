/* env_test.cpp — 循环包络 (loopEnv) 单元验证: p01=0.92 时 env 是否真正缩放非 mode-3 层 alpha */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../dissolve_direct.h"
#include "../dissolve_core.h"
#include "../dissolve_styles.h"
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
    std::vector<float> src((size_t)W*H*4, 0.f);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            size_t i = (size_t)y*W+x;
            src[i*4+0] = 1.f; src[i*4+1] = 1.f; src[i*4+2] = 1.f;
            // 真实形状: 仅在中心矩形不透明 (文字语义)
            src[i*4+3] = (x >= 30 && x < 130 && y >= 30 && y < 130) ? 1.f : 0.f;
        }

    int fails = 0;
    const Preset& pres = kPresets[0];  // 2 Color Stripes: 蓝(0-99) + 源(0-99) + 橙(30-60)
    int nL = std::min(pres.nLayers, 5);
    for (int pct = 40; pct <= 100; pct += 5) {
        DirectFrame fr;
        fr.preset = &pres;
        fr.progress01 = pct / 100.f;
        fr.totalFrames = pres.duration * 30.f;
        fr.explicitFrames = pct * 0.01f * pres.duration * 30.f;
        fr.splatRadius = 10.f;
        fr.rampS = 1.f;
        fr.srcRGBA = src.data();
        fr.noiseFill = buf.noiseMap.data();
        std::vector<float> pts((size_t)nL*2, 0.f), th(nL, 0.f);
        for (int li = 0; li < nL; li++) { pts[(size_t)li*2+0] = 45.f; pts[(size_t)li*2+1] = 45.f; th[li] = 0.f; }
        fr.layerPts = pts.data();
        fr.layerThresh = th.data();
        fr.growthSource = 0;
        // env = 实际渲染用的公式
        float env = 1.f - 1.f * smoothstepField(0.85f, 1.f, pct / 100.f);
        fr.loopEnv = env;
        std::vector<float> cR, cG, cB, cA;
        renderPresetDirect(fr, W, H, cR, cG, cB, cA);
        // maxA 应在 env=1 时 ≈ 0.93 (蓝+文字 src-over), env=0 时 ≈ 0.74 (仅文字)
        float mxA = 0.f;
        for (size_t i = 0; i < (size_t)W*H; i++) if (cA[i] > mxA) mxA = cA[i];
        float expectMax = (env <= 0.001f) ? 0.76f : 1.01f;
        bool ok = mxA >= 0.5f && mxA <= expectMax + 0.01f;
        printf("pct=%3d env=%.3f maxA=%.3f %s\n", pct, env, mxA, ok ? "PASS" : "FAIL");
        if (!ok) fails++;
    }
    // 关键断言: env=0 时 maxA 必须明显小于 env=1 时 (淡出生效)
    DirectFrame fr1, fr2;
    fr1.preset = fr2.preset = &pres;
    fr1.progress01 = fr2.progress01 = 0.92f;
    fr1.totalFrames = fr2.totalFrames = pres.duration * 30.f;
    fr1.explicitFrames = fr2.explicitFrames = 0.92f * pres.duration * 30.f;
    fr1.splatRadius = fr2.splatRadius = 10.f;
    fr1.rampS = fr2.rampS = 1.f;
    fr1.srcRGBA = fr2.srcRGBA = src.data();
    fr1.noiseFill = fr2.noiseFill = buf.noiseMap.data();
    std::vector<float> pts2((size_t)nL*2, 0.f), th2(nL, 0.f);
    for (int li = 0; li < nL; li++) { pts2[(size_t)li*2+0] = 45.f; pts2[(size_t)li*2+1] = 45.f; th2[li] = 0.f; }
    fr1.layerPts = fr2.layerPts = pts2.data();
    fr1.layerThresh = fr2.layerThresh = th2.data();
    fr1.growthSource = fr2.growthSource = 0;
    fr1.loopEnv = 1.f;  // 无淡出
    fr2.loopEnv = 0.f;  // 完全淡出
    std::vector<float> a1, a2, r1, g1, b1, r2, g2, b2;
    renderPresetDirect(fr1, W, H, r1, g1, b1, a1);
    renderPresetDirect(fr2, W, H, r2, g2, b2, a2);
    int lit1 = 0, lit2 = 0;
    for (size_t i = 0; i < (size_t)W*H; i++) {
        if (a1[i] > 0.01f) lit1++;
        if (a2[i] > 0.01f) lit2++;
    }
    // 正确断言: env=0 时填充层全灭 → lit 只剩文字形状像素 (≈100×100=10K)
    //            env=1 时填充覆盖全区域 (160×160=25.6K)
    printf("KEY: env=1 lit=%d (of %d) | env=0 lit=%d → %s\n",
        lit1, W*H, lit2, (lit2 < lit1 / 2) ? "FADE WORKS" : "FADE BROKEN");
    if (!(lit2 < lit1 / 2)) fails++;
    printf(fails ? "ENV_TEST_FAILED (%d)\n" : "ENV_TEST_OK\n", fails);
    return fails ? 1 : 0;
}
