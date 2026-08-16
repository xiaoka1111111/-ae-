/* growth_regress.cpp — 生长动画回归:
 *   1) 预设 0 "(Reveal) 2 Color Stripes": 0->100 进度帧的填充比例单调不减, 终点 ~0.97
 *   2) 全部 30 预设: 进度 0->100 内容比例有生长 (起点低 -> 中途显著 -> 不倒退)
 * 内容比例 = 形状内 (srcAlpha>0.5) 且渲染 alpha>0.05 的像素 / 形状内像素
 *   (alpha>0.5 阈值会误判半透明渐变预设如 Thin Line Cars / Sound Waves, 它们
 *    的渐变 stops 自带 alpha 渐隐, 设计即如此)
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../dissolve_core.h"
#include "../dissolve_styles.h"
#include "../preset_data.h"
#include <cstdio>
#include <vector>
#include <algorithm>

using namespace dissolve;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (cond) { printf("  [PASS] "); } else { printf("  [FAIL] "); g_fail++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static float contentRatio(const Buffers& buf, const float* alpha, size_t inShape,
                          const Preset& pr, float prog, int w, int h) {
    StyleFrame fr; fr.preset = &pr; fr.progress = prog;
    std::vector<float> cR, cG, cB, cA;
    // srcRGBA 必须传带 alpha 的源图 (nullptr = 全画布皆形状, BFS 不裁剪)
    std::vector<float> src((size_t)w*h*4, 0.f);
    for (size_t i = 0; i < (size_t)w*h; i++) { src[i*4+3] = alpha[i]; }
    renderPreset(fr, buf, src.data(), w, h, cR, cG, cB, cA);
    size_t lit = 0;
    for (size_t i = 0; i < (size_t)w*h; i++)
        if (alpha[i] > 0.5f && cA[i] > 0.05f) lit++;
    return (float)lit / (float)inShape;
}

int main() {
    const int W = 160, H = 160;
    // 圆形 alpha 形状 (非对称细节)
    std::vector<float> alpha((size_t)W*H, 0.f);
    for (int y = 20; y < 140; y++)
        for (int x = 20; x < 140; x++) {
            float dx = (x-80)/60.f, dy = (y-80)/60.f;
            if (dx*dx+dy*dy < 1.f) alpha[(size_t)y*W+x] = 1.f;
        }
    Params p;
    Buffers buf;
    generateNoiseMap(p, buf, W, H);
    sobelEdges(alpha.data(), buf, W, H);
    jfaDistance(p, alpha.data(), buf, W, H);
    size_t inShape = 0;
    for (size_t i = 0; i < (size_t)W*H; i++) if (alpha[i] > 0.5f) inShape++;

    // ---- 1) 2CS 单调性 + 终点 (不透明层, alpha>0.5 判定适用) ----
    printf("== 生长回归 1: 2 Color Stripes 单调性 ==\n");
    {
        const Preset& pr = kPresets[0];
        std::vector<float> ratio;
        for (int prog = 0; prog <= 100; prog += 2) {
            float r = contentRatio(buf, alpha.data(), inShape, pr, (float)prog, W, H);
            ratio.push_back(r);
            if (prog % 20 == 0 || prog == 100)
                printf("  prog=%3d%% fill=%.3f\n", prog, r);
        }
        bool mono = true;
        for (size_t i = 1; i < ratio.size(); i++)
            if (ratio[i] < ratio[i-1] - 0.005f) mono = false;
        CHECK(mono, "填充比例随进度单调不减 (容差 0.005)");
        CHECK(ratio.front() < 0.30f, "起点填充比例 < 0.30 (实际 %.3f)", ratio.front());
        CHECK(ratio.back() >= 0.94f && ratio.back() <= 1.00f, "终点 ≈ 0.97±0.03 (实际 %.3f)", ratio.back());
    }

    // ---- 2) 30 预设生长: 起点低 -> 中途显著 -> 峰值前不倒退 ----
    printf("== 生长回归 2: 全部 30 预设生长曲线 ==\n");
    {
        int nPresets = (int)(sizeof(kPresets)/sizeof(kPresets[0]));
        int grown = 0;
        for (int pi = 0; pi < nPresets; pi++) {
            const Preset& pr = kPresets[pi];
            std::vector<float> rs;
            for (int prog = 0; prog <= 100; prog += 10)
                rs.push_back(contentRatio(buf, alpha.data(), inShape, pr, (float)prog, W, H));
            // 峰值及其位置
            float mx = 0.f; int mxAt = 0;
            for (int k = 0; k < (int)rs.size(); k++) if (rs[k] > mx) { mx = rs[k]; mxAt = k; }
            // 峰值前单调 (容差 0.15: 窗口软边/渐变 alpha/blur 柔光外扩可致显著波动),
            // 峰值后回落是窗口关闭语义 (周期收尾淡出)
            bool monoUp = true;
            for (int k = 1; k <= mxAt; k++)
                if (rs[k] < rs[k-1] - 0.15f) monoUp = false;
            bool grows = (rs[0] < 0.25f) && (mx > 0.40f) && monoUp;
            if (grows) grown++;
            if (pi == 15) {
                printf("      FW curve:");
                for (int k = 0; k < (int)rs.size(); k++) printf(" %d%%=%.2f", k*10, rs[k]);
                printf(" mxAt=%d monoUp=%d\n", mxAt, (int)monoUp);
            }
            printf("  [%2d] %-28s r0=%.2f r50=%.2f r100=%.2f peak=%.2f@%d%% %s\n",
                   pi, pr.name, rs[0], rs[5], rs[10], mx, mxAt*10, grows ? "OK" : "BAD");
        }
        CHECK(grown == nPresets, "30/30 预设起点低、中途峰值显著、峰值前单调 (实际 %d/30)", grown);
    }

    // ---- 3) 形状外永不填充 (BFS 只在形状内传播; 进度 100% 也不得溢出) ----
    printf("== 生长回归 3: 形状外不填充 ==\n");
    {
        const Preset& pr = kPresets[0];  // 2 Color Stripes, 层窗口含 [0,99]
        size_t outside = 0;
        for (size_t i = 0; i < (size_t)W*H; i++) if (alpha[i] <= 0.5f) outside++;
        std::vector<float> cR, cG, cB, cA;
        std::vector<float> src((size_t)W*H*4, 0.f);
        for (size_t i = 0; i < (size_t)W*H; i++) { src[i*4+0]=1.f; src[i*4+1]=1.f; src[i*4+2]=1.f; src[i*4+3]=alpha[i]; }
        StyleFrame fr; fr.preset = &pr; fr.progress = 100.f;
        renderPreset(fr, buf, src.data(), W, H, cR, cG, cB, cA);
        size_t litOut = 0;
        for (size_t i = 0; i < (size_t)W*H; i++)
            if (alpha[i] <= 0.5f && cA[i] > 0.05f) litOut++;
        float rOut = (float)litOut / (float)outside;
        printf("  形状外内容比例 @100%%: %.4f (%zu/%zu)\n", rOut, litOut, outside);
        CHECK(rOut < 0.001f, "进度 100%% 时形状外无内容 (实际 %.4f)", rOut);
    }

    printf("\n== %s ==\n", g_fail ? "存在失败" : "全部通过");
    return g_fail ? 1 : 0;
}
