/*
 * test_styles.cpp — 预设图层渲染系统验证
 * 编译: cl /O2 /EHsc test_styles.cpp ..\dissolve_core.cpp ..\dissolve_styles.cpp
 */
#include "../dissolve_core.h"
#include "../dissolve_styles.h"
#include "../preset_data.h"
#include "../preset_names_cn.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <vector>

using namespace dissolve;

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  [PASS] %s\n", msg); \
    else { printf("  [FAIL] %s\n", msg); failures++; } \
} while(0)

int main() {
    printf("== 预设系统验证 (%d 个预设) ==\n", kNumPresets);

    // 1. 预设数据完整性
    printf("[1] 预设数据完整性:\n");
    bool dataOk = true;
    for (int pi = 0; pi < kNumPresets; pi++) {
        const Preset& p = kPresets[pi];
        if (!p.name || p.name[0] == 0) dataOk = false;
        if (p.nLayers <= 0 || p.nLayers > 5) { dataOk = false; printf("    %s: nLayers=%d 异常\n", p.name, p.nLayers); }
        for (int li = 0; li < p.nLayers; li++) {
            const PresetLayer& L = p.layers[li];
            if (L.nStops > 12 || L.nStops < 0) dataOk = false;
            for (int si = 0; si < L.nStops; si++) {
                if (!(L.stops[si].pos >= 0.f && L.stops[si].pos <= 1.f)) dataOk = false;
            }
        }
    }
    CHECK(dataOk, "30 个预设结构完整 (5 层上限/12 色标上限)");

    // 1b. 中文预设名/POPUP 对齐 (分隔符缺失会合并项, 第 30 个预设不可选)
    printf("[1b] 中文预设名对齐:\n");
    {
        bool nameOk = true;
        for (int pi = 0; pi < kNumPresets; pi++) {
            const char* cn = kPresetNamesCN[pi];
            if (!cn || cn[0] == 0) { nameOk = false; break; }
        }
        CHECK(nameOk, "kPresetNamesCN 30 项非空");
        // 按 | 分隔统计 POPUP 项数 (设计列表 None/Reset 前置 -> 32 项)
        int items = 1;
        for (const char* s = kPresetPopupItemsCN; *s; s++) if (*s == '|') items++;
        char itemsMsg[64];
        snprintf(itemsMsg, sizeof(itemsMsg), "POPUP 分隔项数 == %d (实际 %d)", kPresetPopupCount, items);
        CHECK(items == kPresetPopupCount, itemsMsg);
        // 首尾项抽查
        bool edgeOk = true;
        const char* first = kPresetPopupItemsCN;
        const char* last = nullptr;
        for (const char* s = kPresetPopupItemsCN; *s; s++) {
            if (*s == '|') last = s + 1;
        }
        if (!last) edgeOk = false;
        else {
            const char* end = last;
            while (*end) end++;
            char buf[64];
            size_t len = (size_t)(end - last);
            if (len >= sizeof(buf)) edgeOk = false;
            else {
                memcpy(buf, last, len); buf[len] = 0;
                if (strcmp(buf, "柔和阴影") != 0) edgeOk = false;
            }
        }
        (void)first;
        CHECK(edgeOk, "POPUP 末项 = 柔和阴影 (第 30 项可达)");
    }

    // 2. 时间进度 (repeat: duration 内重复 N 次, 周期 = duration/repeat, 周期内 0-100%)
    printf("[2] 时间进度:\n");
    const Preset& dashed = kPresets[3];  // Dashed Source: duration=0.5, repeat=4 -> 周期 0.125s
    float p1 = progressFromTime(dashed, 0.25f);   // 恰好 2 个周期结束 -> 0%
    float p2 = progressFromTime(dashed, 0.30f);   // 0.05s 进入新周期 -> 40%
    float p3 = progressFromTime(dashed, 0.31f);   // 0.06s -> 48%
    printf("    Dashed Source: t=0.25s -> %.1f%%, t=0.30s -> %.1f%%, t=0.31s -> %.1f%%\n", p1, p2, p3);
    CHECK(std::fabs(p1) < 1.f, "0.25s = 周期边界 0%");
    CHECK(std::fabs(p2 - 40.f) < 1.f, "0.30s = 40% (周期 0.125s 内 0.05s)");
    CHECK(p3 > p2, "进度随时间推进");

    // 3. 渐变采样
    printf("[3] 渐变采样:\n");
    float r, g, b, a;
    const PresetLayer& L = kPresets[0].layers[0];  // 2 Color Stripes layer1: 无渐变
    sampleGradient(L.stops, L.nStops, 0.5f, &r, &g, &b, &a);
    // (Reveal) Fire layer1 渐变: 0.5 处介于 [1,0.47,0,1@0.2633] 和 [1,0,0,0@1] 之间
    const PresetLayer& LF = kPresets[7].layers[0];
    sampleGradient(LF.stops, LF.nStops, 0.0f, &r, &g, &b, &a);
    CHECK(std::fabs(r - 1.f) < 0.01f && std::fabs(g - 0.4706f) < 0.01f, "Fire 渐变起点 = 橙色 (1, 0.47, 0)");
    sampleGradient(LF.stops, LF.nStops, 1.0f, &r, &g, &b, &a);
    CHECK(std::fabs(g) < 0.01f, "Fire 渐变终点 = 红色 (1, 0, 0)");

    // 4. 混合模式 ( 跳表 → PF_Xfer 权威映射, A 级依据)
    printf("[4] 混合模式:\n");
    {
        float dr = 0.5f, dg = 0.5f, db = 0.5f, da = 0.f;
        blendPixel(&dr, &dg, &db, &da, 0.0f, 0.5f, 1.0f, 1.0f, BM_SCREEN, 100.f);
        CHECK(da > 0.99f, "Screen = src-over 合成 alpha=1");
        CHECK(std::fabs(dg - 0.75f) < 0.01f, "Screen(0.5,0.5) = 1-0.5*0.5 = 0.75 (PF_Xfer_SCREEN)");
        dr = 0.5f; dg = 0.5f; db = 0.5f; da = 0.f;
        blendPixel(&dr, &dg, &db, &da, 0.0f, 0.5f, 1.0f, 1.0f, BM_MULTIPLY, 100.f);
        CHECK(dg < 0.3f, "Multiply(0.5, 0.5) 变暗 (PF_Xfer_MULTIPLY)");
        dr = 0.5f; dg = 0.5f; db = 0.5f; da = 0.f;
        blendPixel(&dr, &dg, &db, &da, 0.0f, 0.5f, 1.0f, 1.0f, BM_ADD, 100.f);
        CHECK(dg > 0.99f, "Add(0.5, 0.5) 提亮 (PF_Xfer_ADD)");
        dr = 0.3f; dg = 0.3f; db = 0.3f; da = 0.f;
        blendPixel(&dr, &dg, &db, &da, 0.0f, 0.8f, 1.0f, 1.0f, BM_OVERLAY, 100.f);
        CHECK(dg > 0.4f, "Overlay(dst<0.5) 变亮 (PF_Xfer_OVERLAY)");
        // 颜色加深 (经典): 1-(1-B)/S
        dr = 0.5f; dg = 0.5f; db = 0.5f; da = 0.f;
        blendPixel(&dr, &dg, &db, &da, 0.0f, 0.5f, 1.0f, 1.0f, BM_COLOR_BURN, 100.f);
        CHECK(std::fabs(dg) < 0.01f, "ColorBurn(0.5, 0.5) = 1-(0.5/0.5) = 0 (PF_Xfer_COLOR_BURN)");
        // 模版Alpha: dstA *= srcA
        dr = 0.1f; dg = 0.1f; db = 0.1f; da = 0.8f;
        blendPixel(&dr, &dg, &db, &da, 0.0f, 0.5f, 1.0f, 0.5f, BM_STENCIL_ALPHA, 100.f);
        CHECK(da < 0.45f, "StencilAlpha: dstA *= srcA (0.8*0.5=0.4)");
        // 剪影Alpha: dstA *= 1-srcA
        dr = 0.1f; dg = 0.1f; db = 0.1f; da = 0.8f;
        blendPixel(&dr, &dg, &db, &da, 0.0f, 0.5f, 1.0f, 0.5f, BM_SILHOU_ALPHA, 100.f);
        CHECK(std::fabs(da - 0.4f) < 0.01f, "SilhouetteAlpha: dstA *= 1-srcA (0.8*0.5=0.4)");
    }

    // 5. 全预设渲染 (128x128 方块 alpha)
    printf("[5] 全预设渲染:\n");
    const int W = 128, H = 128;
    Params p;
    Buffers buf;
    std::vector<float> alpha((size_t)W*H, 0.f);
    for (int y = 32; y < 96; y++)
        for (int x = 32; x < 96; x++)
            alpha[(size_t)y*W+x] = 1.f;
    generateNoiseMap(p, buf, W, H);
    sobelEdges(alpha.data(), buf, W, H);
    jfaDistance(p, alpha.data(), buf, W, H);

    int badRange = 0, zeroOut = 0;
    for (int pi = 0; pi < kNumPresets; pi++) {
        for (float prog : {0.f, 25.f, 50.f, 75.f, 100.f}) {
            StyleFrame fr;
            fr.preset = &kPresets[pi];
            fr.progress = prog;
            std::vector<float> cR, cG, cB, cA;
            renderPreset(fr, buf, nullptr, W, H, cR, cG, cB, cA);
            bool ok = true;
            float maxA = 0;
            for (size_t i = 0; i < (size_t)W*H; i++) {
                if (!(cR[i] >= 0.f && cR[i] <= 1.f) ||
                    !(cG[i] >= 0.f && cG[i] <= 1.f) ||
                    !(cB[i] >= 0.f && cB[i] <= 1.f) ||
                    !(cA[i] >= 0.f && cA[i] <= 1.f)) { ok = false; break; }
                maxA = std::max(maxA, cA[i]);
            }
            if (!ok) badRange++;
            if (prog == 50.f && maxA < 0.01f && pi != 9 && pi != 24) zeroOut++;  // Love Mist/Echo 有特殊窗口
        }
    }
    CHECK(badRange == 0, "30 预设 x 5 进度: 输出全部在 [0,1]");
    CHECK(zeroOut == 0, "50% 进度时多数预设产生图层内容");

    // 6. 进度推进产生动画差异 (Glow: layer1 窗口 5-10%, 青色 0,1,0.988, ADD 混合)
    printf("[6] 进度动画行为:\n");
    {
        StyleFrame fr;
        fr.preset = &kPresets[4];  // Glow
        // 用彩色源图验证 mode=3 原图显现层
        std::vector<float> src((size_t)W*H*4, 0.f);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                size_t i = ((size_t)y*W+x)*4;
                src[i+0] = 0.2f; src[i+1] = 0.4f; src[i+2] = 0.6f; src[i+3] = 1.f;
            }
        std::vector<float> cR, cG, cB, cA;
        // 数据序渲染语义: 层5 [0,99] 渐变最后画=顶层, alpha 中心 1->边缘 0
        //   -> 中心由顶层渐变主导 (恒定), 环带 (nd~0.5) 随波前推进变化 (生长动画)
        auto colorAt = [&](float prog, int px, int py, float* r, float* g, float* b) {
            fr.progress = prog;
            renderPreset(fr, buf, src.data(), W, H, cR, cG, cB, cA);
            size_t i = (size_t)py*W+px;
            *r = cR[i]; *g = cG[i]; *b = cB[i];
        };
        float r2, g2, b2, r7, g7, b7, r10, g10, b10, r50, g50, b50;
        float er2, eg2, eb2, er50, eg50, eb50;
        colorAt(2.f, 64, 64, &r2, &g2, &b2);
        colorAt(7.5f, 64, 64, &r7, &g7, &b7);
        colorAt(10.5f, 64, 64, &r10, &g10, &b10);
        colorAt(50.f, 64, 64, &r50, &g50, &b50);
        // 环带 (80,64): 方形 32-96 中心 64, maxDist=32, nd=(80-64)/32=0.5
        colorAt(2.f, 80, 64, &er2, &eg2, &eb2);
        colorAt(50.f, 80, 64, &er50, &eg50, &eb50);
        printf("    中心色: 2%%=(%.2f,%.2f,%.2f) 7.5%%=(%.2f,%.2f,%.2f) 10.5%%=(%.2f,%.2f,%.2f) 50%%=(%.2f,%.2f,%.2f)\n",
               r2, g2, b2, r7, g7, b7, r10, g10, b10, r50, g50, b50);
        printf("    环带(80,64): 2%%=(%.2f,%.2f,%.2f) 50%%=(%.2f,%.2f,%.2f)\n",
               er2, eg2, eb2, er50, eg50, eb50);
        // 中心: 顶层渐变起点色 (蓝青色, r 低 b 高) — 数据序渲染语义
        CHECK(b2 > 0.5f && r2 < 0.25f, "中心 = 顶层渐变起点色 (数据序渲染)");
        // 环带: 波前 2% 未到达 (nd=0.5 > 0.02), 50% 已到达 -> 颜色变化 = 生长动画
        CHECK(std::fabs(eg50 - eg2) > 0.05f || std::fabs(eb50 - eb2) > 0.05f,
              "环带随波前推进变化 (生长动画)");
        // 中心恒定 (顶层渐变 alpha=1 覆盖)
        CHECK(std::fabs(b50 - b2) < 0.1f, "中心被顶层渐变覆盖 (恒定)");
    }

    // 7. Fill_GPU 参数生效 (speedOverlay/borderControl/gamma/exposure)
    printf("[7] Fill_GPU 参数:\n");
    {
        Params fp;
        StyleFrame fr;
        fr.preset = &kPresets[0];  // 2 Color Stripes
        fr.progress = 50.f;
        std::vector<float> src7((size_t)W*H*4, 0.f);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                size_t i = ((size_t)y*W+x)*4;
                src7[i+0] = 0.2f; src7[i+1] = 0.4f; src7[i+2] = 0.6f; src7[i+3] = 1.f;
            }
        std::vector<float> cR, cG, cB, cA;
        // 基线 (无 params): 填充区外 = 0
        renderPreset(fr, buf, src7.data(), W, H, cR, cG, cB, cA);
        std::vector<float> baseA = cA;
        // params 但 speed=0/border=0/gamma=1/exposure=1: 应完全等于基线
        fp.speedMapInfluenceF = 0.f; fp.borderInfluenceF = 0.f;
        fp.gammaF = 1.f; fp.exposureF = 1.f;
        fr.params = &fp;
        renderPreset(fr, buf, src7.data(), W, H, cR, cG, cB, cA);
        double d0 = 0; int c0 = 0;
        for (size_t i = 0; i < (size_t)W*H; i++) { d0 += std::fabs(cA[i] - baseA[i]); c0++; }
        d0 /= c0;
        printf("    speed=0/border=0/gamma=1: 平均差=%.5f\n", d0);
        CHECK(d0 < 1e-4, "speed=0/border=0/gamma=1 时输出与基线一致");
        // speed=0.5: fill 为 0/1 二值 + 1px 软边时端到端调制量极小 (设计 coverage 同理);
        // 直接验证 fillComposite 数学 (fill=0.5 软边中间值场景)
        fp.speedMapInfluenceF = 0.5f;
        renderPreset(fr, buf, src7.data(), W, H, cR, cG, cB, cA);
        // 已填充区 (中心) 不被调制: alpha 保持 1
        CHECK(cA[(size_t)64*W+64] > 0.99f, "speed 调制不破坏已填充区 (中心 alpha=1)");
        {
            // fillComposite: fill=0.5, speed=0.7, influence=0.5
            // overlay(0.5,0.7)=0.7 -> v1 = 0.7*0.5 + 0.5*0.5 = 0.6
            std::vector<float> fm9((size_t)W*H, 0.5f), sp9((size_t)W*H, 0.7f), out9((size_t)W*H);
            Params sp;
            sp.speedMapInfluenceF = 0.5f; sp.borderInfluenceF = 0.f;
            sp.gammaF = 1.f; sp.exposureF = 1.f;
            Buffers sb; sb.w = W; sb.h = H; sb.speedMap = sp9;
            fillComposite(sp, sb, fm9.data(), W, H, out9.data());
            printf("    speed=0.5: fill 0.5 + speed 0.7 -> %.3f (期望 0.6)\n", out9[0]);
            CHECK(std::fabs(out9[0] - 0.6f) < 0.01f, "speedOverlay 数学 (overlay(0.5,0.7) mix 0.5)");
        }
        // border=0.5: 直接验证 fillComposite (fill=0.5, border=0.8, influence=0.5)
        {
            std::vector<float> fm10((size_t)W*H, 0.5f), ed10((size_t)W*H, 0.8f), out10((size_t)W*H);
            Params bp;
            bp.speedMapInfluenceF = 0.f; bp.borderInfluenceF = 0.5f;
            bp.gammaF = 1.f; bp.exposureF = 1.f;
            Buffers bb; bb.w = W; bb.h = H; bb.edgeMap = ed10;
            fillComposite(bp, bb, fm10.data(), W, H, out10.data());
            // v2 = 0.8*0.5*0.5 + 0.5*0.5 = 0.45
            printf("    border=0.5: fill 0.5 + border 0.8 -> %.3f (期望 0.45)\n", out10[0]);
            CHECK(std::fabs(out10[0] - 0.45f) < 0.01f, "borderControl 数学 (border*0.5 mix 0.5)");
        }
        // gamma/exposure: 直接验证 fillComposite 数学 (fill 0.5 -> 0.5^2=0.25)
        fp.borderInfluenceF = 0.f; fp.speedMapInfluenceF = 0.f;
        fp.gammaF = 2.f; fp.exposureF = 1.f;
        std::vector<float> fm7((size_t)W*H, 0.5f), outF7((size_t)W*H);
        fillComposite(fp, buf, fm7.data(), W, H, outF7.data());
        printf("    gamma=2: fill 0.5 -> %.3f\n", outF7[0]);
        CHECK(std::fabs(outF7[0] - 0.25f) < 0.01f, "gamma=2: 0.5 -> 0.25");
        fp.gammaF = 1.f; fp.exposureF = 2.f;
        fillComposite(fp, buf, fm7.data(), W, H, outF7.data());
        printf("    exposure=2: fill 0.5 -> %.3f\n", outF7[0]);
        CHECK(std::fabs(outF7[0] - 1.0f) < 0.01f, "exposure=2: 0.5 -> 1.0 (clamp)");
        fr.params = nullptr;
    }

    // 8. 圆点笔刷种子 (设计  实现)
    printf("[8] 圆点笔刷种子:\n");
    {
        const int W8 = 96, H8 = 96;
        std::vector<float> mask;
        float pts1[2] = { 48.f, 48.f };
        brushSeedMask(pts1, 1, 10.f, W8, H8, mask);
        // 中心 = 1, 半径内 = 1, 软边环, 半径外 = 0
        CHECK(mask[(size_t)48*W8+48] > 0.99f, "笔刷中心 factor=1");
        CHECK(mask[(size_t)50*W8+48] > 0.99f, "半径内 factor=1 (r=2 < 9)");
        CHECK(mask[(size_t)48*W8+57] > 0.99f, "半径 9 内 factor=1");
        // 软边: r in (9,10] -> factor=(100-sq)/19 > 0; 对角 (49,57): sq=82 -> 0.947
        CHECK(mask[(size_t)57*W8+49] > 0.5f && mask[(size_t)57*W8+49] < 1.0f,
              "软边环 factor in (0,1) (对角 sq=82 -> 0.947)");
        CHECK(mask[(size_t)48*W8+58] < 0.01f, "半径 10 边界 factor=0 (设计 sq_rad 语义)");
        CHECK(mask[(size_t)48*W8+63] < 0.01f, "半径外 factor=0 (r=15)");
        // 多点取 max
        float pts2[4] = { 30.f, 30.f, 70.f, 70.f };
        brushSeedMask(pts2, 2, 8.f, W8, H8, mask);
        CHECK(mask[(size_t)30*W8+30] > 0.99f && mask[(size_t)70*W8+70] > 0.99f,
              "双点笔刷双中心=1");
        CHECK(mask[(size_t)48*W8+48] < 0.01f, "双点之间 (距各点>8) = 0");
        // 多源 BFS 传播: 双种子时波前从两点同时推进
        std::vector<float> src8((size_t)W8*H8*4, 0.f);
        for (int y = 0; y < H8; y++)
            for (int x = 0; x < W8; x++)
                src8[((size_t)y*W8+x)*4+3] = 1.f;  // 全画布形状
        Params p8;
        Buffers buf8;
        generateNoiseMap(p8, buf8, W8, H8);
        StyleFrame fr8;
        fr8.preset = &kPresets[0];
        fr8.progress = 30.f;
        fr8.seedMask = mask.data();
        std::vector<float> cR, cG, cB, cA;
        renderPreset(fr8, buf8, src8.data(), W8, H8, cR, cG, cB, cA);
        size_t lit8 = 0;
        for (size_t i = 0; i < (size_t)W8*H8; i++) if (cA[i] > 0.05f) lit8++;
        float r8 = (float)lit8 / (float)(W8*H8);
        // 30% 传播: 双种子各 0.3*maxDist=42px 半径, 96x96 画布覆盖 ~60-70%
        printf("    双种子 @30%% 内容比例=%.3f\n", r8);
        CHECK(r8 > 0.40f && r8 < 0.85f, "双种子 30%% 传播内容比例合理 (0.40-0.85)");
        // 自动选点对照: 单种子 (中心) @30% 比例更小
        fr8.seedMask = nullptr;
        renderPreset(fr8, buf8, src8.data(), W8, H8, cR, cG, cB, cA);
        size_t lit8b = 0;
        for (size_t i = 0; i < (size_t)W8*H8; i++) if (cA[i] > 0.05f) lit8b++;
        float r8b = (float)lit8b / (float)(W8*H8);
        printf("    自动选点 @30%% 内容比例=%.3f\n", r8b);
        CHECK(r8b < r8 + 0.05f, "双种子传播 ≥ 单种子 (多源更快)");
    }

    // 9. 速度图生成 (设计 SpeedMap 内核 / 实现;
    //    实证: channel 仅 亮度|Alpha 2 项, 主通道写入 R 位置; mode∉{1,2}=0)
    printf("[9] 速度图生成:\n");
    {
        const int W9 = 4, H9 = 1;
        // 像素: R=0.2 G=0.4 B=0.6 A=1.0
        std::vector<float> src9 = { 0.2f, 0.4f, 0.6f, 1.0f };
        std::vector<float> sm;
        // mode2 ch=Alpha: A/scale = 1.0 (scale=1)
        speedMapFromSource(src9.data(), W9, H9, 2, 1, 1.f, sm);
        printf("    mode2 ch=Alpha: %.4f (期望 1.0000)\n", sm[0]);
        CHECK(std::fabs(sm[0] - 1.0f) < 0.01f, "mode2: 通道值/scale");
        // mode2 Luma: 0.299*0.2+0.587*0.4+0.114*0.6 = 0.3628
        speedMapFromSource(src9.data(), W9, H9, 2, 0, 1.f, sm);
        printf("    mode2 ch=Luma: %.4f (期望 0.3628)\n", sm[0]);
        CHECK(std::fabs(sm[0] - 0.3628f) < 0.01f, "mode2 Luma: 亮度 (0.299/0.587/0.114)");
        // mode1 ch=Alpha: A*avg(G,B,A)/scale^2 = 1.0*(0.4+0.6+1.0)/3 = 0.6667
        speedMapFromSource(src9.data(), W9, H9, 1, 1, 1.f, sm);
        printf("    mode1 ch=Alpha: %.4f (期望 0.6667)\n", sm[0]);
        CHECK(std::fabs(sm[0] - 0.6667f) < 0.01f, "mode1: 通道×其余平均/scale^2");
        // scale=2: 0.6667/4 = 0.1667
        speedMapFromSource(src9.data(), W9, H9, 1, 1, 2.f, sm);
        CHECK(std::fabs(sm[0] - 0.1667f) < 0.01f, "mode1 scale=2: /scale^2");
        // 阈值: Luma 0.0001 -> 0 (阈值 0.001)
        std::vector<float> srcT = { 0.0001f, 0.f, 0.f, 1.f };
        speedMapFromSource(srcT.data(), W9, H9, 2, 0, 1.f, sm);
        CHECK(sm[0] == 0.f, "阈值 0.001 清零");
        // clamp: scale=0.5 -> main/0.5 = 2.0 -> 1.0
        std::vector<float> srcC = { 0.f, 0.f, 0.f, 1.f };
        speedMapFromSource(srcC.data(), W9, H9, 2, 1, 0.5f, sm);
        CHECK(sm[0] == 1.f, "clamp [0,1]");
        // mode=3 (无第三模式): 输出恒 0 (实证)
        speedMapFromSource(src9.data(), W9, H9, 3, 1, 1.f, sm);
        CHECK(sm[0] == 0.f, "mode3 无第三模式 -> 0");
    }

    printf("\n%s (%d 失败)\n", failures == 0 ? "== 全部通过 ==" : "== 有失败 ==", failures);
    return failures;
}

