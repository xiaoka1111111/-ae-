/* debug_composite.cpp — 调试 Short Wave / Soft Shadow 中心黑屏 */
#include "../dissolve_core.h"
#include "../dissolve_styles.h"
#include "../preset_data.h"
#include <cstdio>
#include <vector>

using namespace dissolve;

int main() {
    const int W = 128, H = 128;
    std::vector<float> src((size_t)W*H*4, 0.f);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            size_t i = ((size_t)y*W+x)*4;
            src[i+0]=0.9f; src[i+1]=0.4f; src[i+2]=0.2f; src[i+3]=1.f;
        }
    std::vector<float> alpha((size_t)W*H, 0.f);
    for (int y = 32; y < 96; y++)
        for (int x = 32; x < 96; x++)
            alpha[(size_t)y*W+x] = 1.f;

    Params p;
    Buffers buf;
    generateNoiseMap(p, buf, W, H);
    sobelEdges(alpha.data(), buf, W, H);
    jfaDistance(p, alpha.data(), buf, W, H);

    for (int pi : {18, 29}) {
        const Preset& pres = kPresets[pi];
        printf("== preset %d: %s ==\n", pi, pres.name);
        printf("  nLayers=%d compOver=%d\n", pres.nLayers, pres.compOverOriginal);
        for (int li = 0; li < pres.nLayers; li++) {
            const PresetLayer& L = pres.layers[li];
            printf("  layer%d: order=%d start=%.1f end=%.1f mode=%d nStops=%d gm=%d grow=%.1f overlay=%d op=%.1f blur=%.1f\n",
                   li, L.order, L.start, L.end, L.mode, L.nStops, L.gradientMode,
                   L.grow, L.overlayMode, L.overlayOpacity, L.blur);
            for (int s = 0; s < L.nStops; s++)
                printf("    stop%d: (%.2f,%.2f,%.2f,a=%.2f)@%.3f\n", s,
                       L.stops[s].r, L.stops[s].g, L.stops[s].b, L.stops[s].a, L.stops[s].pos);
        }
        for (float prog : {0.f, 25.f, 50.f, 75.f}) {
            StyleFrame fr;
            fr.preset = &pres;
            fr.progress = prog;
            std::vector<float> cR, cG, cB, cA;
            renderPreset(fr, buf, src.data(), W, H, cR, cG, cB, cA);
            size_t ci = (size_t)64*W+64;
            printf("  prog=%.0f: center layerRGB=(%.3f,%.3f,%.3f) a=%.3f | center dist=%.3f\n",
                   prog, cR[ci], cG[ci], cB[ci], cA[ci], buf.distField[64*W+64]);
        }
    }
    return 0;
}
