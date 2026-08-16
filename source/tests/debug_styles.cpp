// debug_styles.cpp — Glow 预设各层调试
#include "../dissolve_core.h"
#include "../dissolve_styles.h"
#include "../preset_data.h"
#include <cstdio>
#include <vector>

using namespace dissolve;

int main() {
    const int W = 128, H = 128;
    Params p;
    Buffers buf;
    std::vector<float> alpha((size_t)W*H, 0.f);
    for (int y = 32; y < 96; y++)
        for (int x = 32; x < 96; x++)
            alpha[(size_t)y*W+x] = 1.f;
    generateNoiseMap(p, buf, W, H);
    printf("generateNoiseMap 后: noiseMap[0]=%.3f\n", buf.noiseMap[0]);
    jfaDistance(p, alpha.data(), buf, W, H);

    // 打印 distField 中心/边缘
    size_t c = (size_t)64*W+64, e = (size_t)32*W+64;
    printf("distField: 中心=%.4f 边缘=%.4f\n", buf.distField[c], buf.distField[e]);
    // 统计
    size_t nSeeds = 0, nDist = 0;
    for (size_t i = 0; i < (size_t)W*H; i++) {
        if (buf.jfaSeeds[i] != 0xFFFFFFFFu) nSeeds++;
        if (buf.distField[i] > 0.001f) nDist++;
    }
    printf("jfaSeeds 非空: %zu/%zu, distField 非零: %zu/%zu\n", nSeeds, (size_t)W*H, nDist, (size_t)W*H);
    printf("alpha 非零: %zu\n", [&]{ size_t n=0; for (size_t i=0;i<(size_t)W*H;i++) if (alpha[i]>0.5f) n++; return n; }());

    const Preset& G = kPresets[4];  // Glow
    printf("Glow 层配置:\n");
    for (int li = 0; li < G.nLayers; li++) {
        const PresetLayer& L = G.layers[li];
        printf("  layer%d: order=%d start=%.1f end=%.1f mode=%d color=(%.2f,%.2f,%.2f,%.2f) overlay=%d op=%.0f blur=%.1f grow=%.1f stops=%d\n",
               li, L.order, L.start, L.end, L.mode, L.color[0], L.color[1], L.color[2], L.color[3],
               L.overlayMode, L.overlayOpacity, L.blur, L.grow, L.nStops);
    }

    // 手动模拟 2% 时各层
    float prog = 2.f;
    for (int li = 0; li < G.nLayers; li++) {
        const PresetLayer& L = G.layers[li];
        float start = L.start, end = std::max(L.end, start + 1.f);
        float t_local = (prog - start) / (end - start);
        bool active = (prog >= start && prog <= end);
        bool isOut = t_local > 1.f;
        float wave = buf.distField[c];
        float maxD = 0;
        for (size_t i = 0; i < (size_t)W*H; i++) maxD = std::max(maxD, buf.distField[i]);
        wave /= maxD;
        float wA = 0;
        if (active || isOut) {
            float tt = std::min(t_local, 1.f);
            float soft = 0.04f;
            wA = smoothstepField(tt - soft, tt + soft, wave);
        }
        printf("  2%%: layer%d t_local=%.2f active=%d wA(中心)=%.3f\n", li, t_local, active, wA);
    }
    return 0;
}
