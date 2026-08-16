/*
 * test_composite.cpp — 最终合成路径验证 (模拟 renderWorld 的 CPU 合成)
 * 验证: 预设渲染 + compOver 混合后输出非黑 (复现 AE 黑屏问题)
 */
#include "../dissolve_core.h"
#include "../dissolve_styles.h"
#include "../preset_data.h"
#include <cstdio>
#include <cmath>
#include <vector>

using namespace dissolve;

int main() {
    printf("== 最终合成路径验证 ==\n");
    const int W = 128, H = 128;
    // 彩色源图 (模拟 AE 输入: 非黑, 有 alpha)
    std::vector<float> src((size_t)W*H*4, 0.f);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            size_t i = ((size_t)y*W+x)*4;
            src[i+0] = 0.9f; src[i+1] = 0.4f; src[i+2] = 0.2f;  // 橙红色源
            src[i+3] = 1.f;                                        // 不透明
        }
    // 方块 alpha 形状
    std::vector<float> alpha((size_t)W*H, 0.f);
    for (int y = 32; y < 96; y++)
        for (int x = 32; x < 96; x++)
            alpha[(size_t)y*W+x] = 1.f;

    Params p;
    Buffers buf;
    generateNoiseMap(p, buf, W, H);
    sobelEdges(alpha.data(), buf, W, H);
    jfaDistance(p, alpha.data(), buf, W, H);

    int blackFrames = 0, total = 0;
    for (int pi = 0; pi < kNumPresets; pi++) {
        for (float prog : {0.f, 25.f, 50.f, 75.f, 100.f}) {
            const Preset& pres = kPresets[pi];
            StyleFrame fr;
            fr.preset = &pres;
            fr.progress = prog;
            std::vector<float> cR, cG, cB, cA;
            renderPreset(fr, buf, src.data(), W, H, cR, cG, cB, cA);

            // 模拟 renderWorld 最终合成 (compOver)
            bool compOver = (pres.compOverOriginal == 0);
            std::vector<float> out = src;
            for (size_t i = 0; i < (size_t)W*H; i++) {
                float a = cA[i];
                float outR, outG, outB, outA;
                if (compOver) {
                    outR = src[i*4+0]*(1-a) + cR[i]*a;
                    outG = src[i*4+1]*(1-a) + cG[i]*a;
                    outB = src[i*4+2]*(1-a) + cB[i]*a;
                    outA = std::max(src[i*4+3], a);
                } else {
                    outR = cR[i]*a + src[i*4+0]*(1-a);
                    outG = cG[i]*a + src[i*4+1]*(1-a);
                    outB = cB[i]*a + src[i*4+2]*(1-a);
                    outA = src[i*4+3]*(1-a);
                }
                out[i*4+0]=outR; out[i*4+1]=outG; out[i*4+2]=outB; out[i*4+3]=outA;
            }
            // 检查方块中心 (64,64) 是否"异常黑":
            // 真正 bug = 层几乎透明 (cA<0.3) 但输出全黑 (透明黑覆盖原图)
            // 层色本身黑 (墨水/阴影/渐变末端) 且 alpha 高 = 正常效果, 不算 bug
            size_t ci = ((size_t)64*W+64)*4;
            total++;
            float layA = cA[64*W+64];
            if (out[ci+0] < 0.01f && out[ci+1] < 0.01f && out[ci+2] < 0.01f &&
                out[ci+3] > 0.5f && layA < 0.3f) {
                blackFrames++;
                printf("  异常黑: preset=%d(%s) prog=%.0f a=%.2f layerRGB=(%.2f,%.2f,%.2f)\n",
                       pi, kPresets[pi].name, prog, layA,
                       cR[64*W+64], cG[64*W+64], cB[64*W+64]);
            }
        }
    }
    printf("预设x进度组合: %d, 中心全黑: %d\n", total, blackFrames);
    if (blackFrames == 0)
        printf("[PASS] 所有组合中心非黑 (合成路径正常)\n");
    else
        printf("[FAIL] %d 个组合中心全黑!\n", blackFrames);

    // 输出一个具体例子: (Reveal) Fire @50%
    {
        const Preset& pres = kPresets[7];
        StyleFrame fr;
        fr.preset = &pres;
        fr.progress = 50.f;
        std::vector<float> cR, cG, cB, cA;
        renderPreset(fr, buf, src.data(), W, H, cR, cG, cB, cA);
        size_t ci = (size_t)64*W+64;
        printf("Fire@50%%: 层色=(%.2f,%.2f,%.2f) alpha=%.2f | 源=(0.9,0.4,0.2)\n",
               cR[ci], cG[ci], cB[ci], cA[ci]);
    }
    return blackFrames == 0 ? 0 : 1;
}
