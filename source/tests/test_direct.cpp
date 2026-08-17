/* test_direct.cpp — dissolve_direct 实现内核单元测试 (对照 A 级公式) */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../dissolve_direct.h"
#include <cstdio>
#include <cmath>
#include <vector>
using namespace dissolve;

static int failures = 0;
#define CHECK(cond, msg) do { if (cond) printf("  [PASS] %s\n", msg); \
    else { printf("  [FAIL] %s\n", msg); failures++; } } while (0)

int main() {
    // 1. 圆点 splat: 软边圆公式 (0x19372-0x1960A)
    printf("[1] 圆点 splat:\n");
    {
        const int W = 32, H = 32;
        std::vector<float> fill;
        float pts[2] = { 16.f, 16.f };     // 圆心 (16,16)
        float thresh[1] = { 0.5f };        // 阈值 0.5
        // r=5: d²≤25 全 1; (5-1)²=16 < d² ≤ 25 软边线性
        splatFillMap(pts, thresh, 1, 5.f, 0.6f, W, H, 0.f, 0.f, fill);
        // 中心 (16,16): d²=(0.5)²+(0.5)²=0.5 ≤ 16 → factor=1
        CHECK(fill[(size_t)16*W+16] == 1.f, "圆心 factor=1");
        // (16,20): d²=(0.5)²+(4.5)²=20.5 ∈ (16,25] → (25-20.5)/(25-16)=0.5
        float expect = (25.f - 20.5f) / 9.f;
        float got = fill[(size_t)20*W+16];
        printf("    (16,20): got=%.3f expect=%.3f\n", got, expect);
        CHECK(std::fabs(got - expect) < 0.01f, "软边线性 (r²−d²)/(r²−(r−1)²)");
        // 半径外 (16,28): d²=(0.5+11.5)² > 25 → 0
        CHECK(fill[(size_t)28*W+16] == 0.f, "半径外为 0");
        // 阈值 gating: threshold=0.9 > prog=0.6 → 不画
        std::vector<float> fill2;
        float th2[1] = { 0.9f };
        splatFillMap(pts, th2, 1, 5.f, 0.6f, W, H, 0.f, 0.f, fill2);
        float mx = 0.f;
        for (size_t i = 0; i < fill2.size(); i++) mx = std::max(mx, fill2[i]);
        CHECK(mx == 0.f, "阈值 > 进度时跳过 (硬阈值 gating)");
        // max 累积: 两个重叠圆取最大
        std::vector<float> fill3;
        float pts2[4] = { 16.f, 16.f, 18.f, 16.f };
        float th3[2] = { 0.f, 0.f };
        splatFillMap(pts2, th3, 2, 5.f, 1.f, W, H, 0.f, 0.f, fill3);
        CHECK(fill3[(size_t)16*W+16] == 1.f, "max 累积 (多圆重叠)");
    }

    // 2. 膨胀核: 3x3 max + 满像素保留 (0x369D0/0x36BB0)
    printf("[2] 膨胀核:\n");
    {
        const int W = 7, H = 7;
        std::vector<float> f((size_t)W*H, 0.f), out;
        f[(size_t)3*W+3] = 1.f;   // 中心 1
        f[(size_t)3*W+4] = 0.5f;  // 右边 0.5
        dilateMaxField(f, out, W, H, 1);
        CHECK(out[(size_t)3*W+2] == 1.f, "膨胀 1 次: 左邻取 1");
        CHECK(out[(size_t)3*W+3] == 1.f, "已满像素保留 1 (不回落)");
        CHECK(out[(size_t)3*W+4] == 1.f, "未满像素取邻域最大 (0.5→1)");
        CHECK(out[(size_t)3*W+5] == 0.5f, "次邻域取 0.5");
        CHECK(out[(size_t)0] == 0.f, "远处仍 0");
        // 迭代 2 次: 传播 2px
        dilateMaxField(f, out, W, H, 2);
        CHECK(out[(size_t)3*W+1] == 1.f, "膨胀 2 次: 2px 到达");
        CHECK(std::fabs(out[(size_t)3*W+6] - 0.5f) < 0.001f, "2 次后 0.5 传播 2px 到 (3,6)");
    }

    // 3. 斜坡内核 0x30B80: out = clamp(fill·S + 0.5·S·(p−0.5) + 2.5·p − 1, 0, 1)
    printf("[3] 斜坡内核 0x30B80:\n");
    {
        // S=2, fill=0.5, p=0.5: 0.5·2 + 0.5·2·0 + 1.25 − 1 = 1.25 → clamp 1.0
        float v = rampKernel30B80(0.5f, 2.f, 0.5f);
        printf("    S=2 fill=.5 p=.5: got=%.3f expect=1.000\n", v);
        CHECK(std::fabs(v - 1.f) < 0.001f, "S=2 fill=.5 p=.5 → 1.0");
        // S=1, fill=0, p=0: 0 + 0.5·1·(−0.5) + 0 − 1 = −1.25 → clamp 0
        v = rampKernel30B80(0.f, 1.f, 0.f);
        CHECK(std::fabs(v - 0.f) < 0.001f, "S=1 fill=0 p=0 → 0 (clamp 下界)");
        // S=1, fill=1, p=1: 1 + 0.25 + 2.5 − 1 = 2.75 → clamp 1
        v = rampKernel30B80(1.f, 1.f, 1.f);
        CHECK(std::fabs(v - 1.f) < 0.001f, "S=1 fill=1 p=1 → 1 (clamp 上界)");
    }

    // 4. overlay+lerp 0x30E50: o = dst<0.5?2ds:1−2(1−d)(1−s); lerp(o,dst,w)
    printf("[4] overlay+lerp 0x30E50:\n");
    {
        // dst=0.25, src=0.5: o=2·0.25·0.5=0.25; w=1 → 0.25
        float v = overlayLerp30E50(0.25f, 0.5f, 1.f);
        CHECK(std::fabs(v - 0.25f) < 0.001f, "dst<0.5: 2·dst·src");
        // dst=0.75, src=0.5: o=1−2·0.25·0.5=0.75; w=1 → 0.75
        v = overlayLerp30E50(0.75f, 0.5f, 1.f);
        CHECK(std::fabs(v - 0.75f) < 0.001f, "dst>=0.5: 1−2(1−d)(1−s)");
        // w=0 → 原值
        v = overlayLerp30E50(0.25f, 0.9f, 0.f);
        CHECK(std::fabs(v - 0.25f) < 0.001f, "w=0 → dst 原值");
        // w=0.5 → 线性混合
        v = overlayLerp30E50(0.25f, 0.5f, 0.5f);
        CHECK(std::fabs(v - 0.25f) < 0.001f, "w=0.5 → lerp 中点");
    }

    // 5. 四边形扭曲 SpeedMap (0x332C0/0x335C0)
    printf("[5] 四边形扭曲 SpeedMap:\n");
    {
        const int SW = 4, SH = 4;
        // 源: (0,0)=R0.2G0.4B0.6A1.0, 其余 0
        std::vector<float> src((size_t)SW*SH*4, 0.f);
        src[0] = 0.2f; src[1] = 0.4f; src[2] = 0.6f; src[3] = 1.f;
        // 恒等 quad: P0=(0,0) P1=(4,0) P2=(0,0) P3=(4,4)? 用简单恒等: 输出(x,y)→源(x,y)
        float quad[8] = { 0.f, 0.f, 4.f, 4.f, 0.f, 0.f, 4.f, 4.f };
        std::vector<float> sm;
        // mode2: R/scale, scale=1 → (0,0) 处 R=0.2
        quadWarpSpeedMap(src.data(), SW, SH, quad, 0.f, 0.f, 2, 1.f, SW, SH, sm);
        CHECK(std::fabs(sm[0] - 0.2f) < 0.001f, "恒等映射 mode2: R/scale=0.2");
        // 位移 dispX=1: 输出(0,0) 采样源(1,0)=0 → 0; 输出(1,0) 采样源(0,0)=0.2
        quadWarpSpeedMap(src.data(), SW, SH, quad, 1.f, 0.f, 2, 1.f, SW, SH, sm);
        CHECK(sm[0] == 0.f, "位移后 (0,0) 采样源(1,0) → 0");
        CHECK(std::fabs(sm[1] - 0.2f) < 0.001f, "位移后 (1,0) 采样源(0,0) → 0.2");
        // mode1: R·(G+B+A)/3/scale² = 0.2·(0.4+0.6+1)/3 = 0.1333
        quadWarpSpeedMap(src.data(), SW, SH, quad, 0.f, 0.f, 1, 1.f, SW, SH, sm);
        printf("    mode1: got=%.4f expect=0.1333\n", sm[0]);
        CHECK(std::fabs(sm[0] - 0.1333f) < 0.001f, "mode1: R·(G+B+A)/(3·scale²)");
        // 阈值: R=0.0005 → 0
        std::vector<float> srcT((size_t)SW*SH*4, 0.f);
        srcT[0] = 0.0005f;
        quadWarpSpeedMap(srcT.data(), SW, SH, quad, 0.f, 0.f, 2, 1.f, SW, SH, sm);
        CHECK(sm[0] == 0.f, "阈值 0.001 清零");
        // mode3 → 恒 0
        quadWarpSpeedMap(src.data(), SW, SH, quad, 0.f, 0.f, 3, 1.f, SW, SH, sm);
        CHECK(sm[0] == 0.f, "mode3 无第三模式 → 0");
    }

    // 6. renderPresetDirect 管线: 硬阈值 gating + splat 传播 + 合成
    printf("[6] renderPresetDirect 管线:\n");
    {
        // 构造单层预设: mode=1 实色红, start=0
        Preset pres;
        pres.name = "direct-test";
        pres.duration = 1.f; pres.repeat = 1; pres.compOverOriginal = 0;
        pres.nLayers = 1;
        pres.layers[0].order = 1; pres.layers[0].start = 0.f; pres.layers[0].end = 99.f;
        pres.layers[0].mode = 1;
        pres.layers[0].color[0] = 1.f; pres.layers[0].color[1] = 0.f;
        pres.layers[0].color[2] = 0.f; pres.layers[0].color[3] = 1.f;
        pres.layers[0].overlayOpacity = 100.f;
        pres.layers[0].overlayMode = 1;  // 正常 (0x3D830 权威映射 1-based)
        const int W = 32, H = 32;
        DirectFrame fr;
        fr.preset = &pres;
        fr.progress01 = 0.5f;
        fr.totalFrames = 100.f;
        fr.splatRadius = 5.f;
        fr.rampS = 1.f;
        // 点在中心 (16,16), 阈值 0
        float pts[2] = { 16.f, 16.f };
        float th[1] = { 0.f };
        fr.layerPts = pts; fr.layerThresh = th;
        std::vector<float> cR, cG, cB, cA;
        renderPresetDirect(fr, W, H, cR, cG, cB, cA);
        // p01=0.5, total=100 → frame=49; 传播半径 5 的 splat + 49 帧膨胀 ≈ 全图
        // 中心应该被覆盖; 层色应用 (宿主颜色填充语义, 复刻模拟):
        //   mode==1 实色层 → 层色 (红)
        size_t ci = (size_t)16*W+16;
        printf("    center: R=%.2f G=%.2f B=%.2f A=%.2f\n", cR[ci], cG[ci], cB[ci], cA[ci]);
        CHECK(cR[ci] > 0.5f, "中心被填充 (splat+传播)");
        CHECK(cR[ci] > cG[ci] + 0.5f, "实色层显示层色 (红) — 宿主颜色填充语义");
        CHECK(cA[ci] > 0.f, "填充区 alpha>0");
        // 阈值 gating: 阈值 0.6 > p01=0.5 → 该层不激活 → 全空
        float th2[1] = { 0.6f };
        fr.layerThresh = th2;
        std::vector<float> cR2, cG2, cB2, cA2;
        renderPresetDirect(fr, W, H, cR2, cG2, cB2, cA2);
        float mx2 = 0.f;
        for (size_t i = 0; i < cA2.size(); i++) mx2 = std::max(mx2, cA2[i]);
        CHECK(mx2 == 0.f, "阈值 > 进度: 层不激活 (硬阈值 gating)");
        // 早帧: p01=0.02 → frame=1 → 只有 splat 半径+1 帧传播 → 角落无内容
        fr.layerThresh = th;
        fr.progress01 = 0.02f;
        std::vector<float> cR3, cG3, cB3, cA3;
        renderPresetDirect(fr, W, H, cR3, cG3, cB3, cA3);
        CHECK(cA3[0] == 0.f, "早帧角落无内容 (传播未到达)");
        CHECK(cA3[ci] > 0.f, "早帧 splat 区域有内容");
    }

    // 7. 生长来源分派 (param 9 A 级): 噪波 / 图层
    printf("[7] 生长来源分派:\n");
    {
        const int W = 32, H = 32;
        Preset pres;
        pres.name = "gs-test";
        pres.duration = 1.f; pres.repeat = 1; pres.compOverOriginal = 0;
        pres.nLayers = 1;
        pres.layers[0].mode = 1;
        pres.layers[0].start = 0.f; pres.layers[0].end = 99.f;
        pres.layers[0].color[0] = 1.f; pres.layers[0].color[1] = 1.f;
        pres.layers[0].color[2] = 1.f; pres.layers[0].color[3] = 1.f;
        pres.layers[0].overlayOpacity = 100.f;
        // 噪波场 (0-1)
        std::vector<float> noise((size_t)W*H, 0.5f);
        for (size_t i = 0; i < noise.size(); i++)
            noise[i] = ((i * 7) % 100) / 100.f;
        // 源图 (旧 7b 语义: 灰底源图 — 新语义下种子来自源图 luma, 不再使用)

        // 7a. 噪波生长 [重构 2026-08-18]: 种子 = ramp(噪声,S,p)>0.01 的像素
        //     (噪波白区作起始点, ramp 含进度项 → 种子随生长出现), BFS 扩散
        DirectFrame fr;
        fr.preset = &pres;
        fr.progress01 = 0.5f;
        fr.rampS = 1.f;
        fr.growthSource = 1;
        fr.noiseFill = noise.data();
        fr.totalFrames = 100.f;
        fr.explicitFrames = 0.f;   // 未传播: 只留 ramp 阈值种子
        std::vector<float> cR, cG, cB, cA;
        renderPresetDirect(fr, W, H, cR, cG, cB, cA);
        size_t lit = 0;
        for (size_t i = 0; i < cA.size(); i++) if (cA[i] > 0.f) lit++;
        printf("    噪波种子(帧0,p=0.5): 内容像素=%zu/%zu\n", lit, (size_t)W*H);
        // p01=0.5: ramp = noise+1.25−1 = noise+0.25 > 0.01 → 全部噪波值 (0..0.99) 都成种子
        CHECK(lit > (size_t)W*H*3/4, "噪波种子: p=0.5 时 ramp>0.01 覆盖大部");
        // 低进度: ramp = noise−1.1 ≤ 0 → 无种子 (噪波随生长逐步出现)
        fr.progress01 = 0.05f;
        renderPresetDirect(fr, W, H, cR, cG, cB, cA);
        lit = 0;
        for (size_t i = 0; i < cA.size(); i++) if (cA[i] > 0.f) lit++;
        printf("    噪波种子(帧0,p=0.05): 内容像素=%zu/%zu\n", lit, (size_t)W*H);
        CHECK(lit == 0, "低进度无种子 (ramp 输出全 0, 一边生长一边出现)");
        // 满帧传播: 从种子扩散覆盖全场
        fr.progress01 = 0.5f;
        fr.explicitFrames = 100.f;
        renderPresetDirect(fr, W, H, cR, cG, cB, cA);
        lit = 0;
        for (size_t i = 0; i < cA.size(); i++) if (cA[i] > 0.f) lit++;
        printf("    噪波生长(满帧): 内容像素=%zu/%zu\n", lit, (size_t)W*H);
        CHECK(lit > (size_t)W*H*3/4, "噪波生长满帧覆盖大部 (BFS 从噪波种子扩散)");

        // 7b. 图层生长 [重构 2026-08-18]: 种子 = 源图 luma>0.05 像素 (白区作起始点)
        //     源图: 黑底 + 中心 9×9 白盘 → 种子=白盘, BFS 扩散
        std::vector<float> src2((size_t)W*H*4, 0.f);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float d = std::max(std::abs(x-16), std::abs(y-16));
                if (d <= 4.f) {
                    size_t i = (size_t)y*W+x;
                    src2[i*4+0]=1.f; src2[i*4+1]=1.f; src2[i*4+2]=1.f; src2[i*4+3]=1.f;
                }
            }
        fr.growthSource = 2;
        fr.srcRGBA = src2.data();
        fr.noiseFill = nullptr;
        fr.progress01 = 0.5f;
        fr.totalFrames = 100.f;
        fr.explicitFrames = 0.f;   // 未传播: 只有白盘种子 (+1px 软边)
        renderPresetDirect(fr, W, H, cR, cG, cB, cA);
        lit = 0;
        for (size_t i = 0; i < cA.size(); i++) if (cA[i] > 0.f) lit++;
        printf("    图层种子(帧0): 内容像素=%zu (期望≈白盘 81)\n", lit);
        CHECK(lit > 40 && lit < 200, "图层生长种子 = 源图白盘 (luma>0.05)");
        // 满帧: 从白盘扩散覆盖全场
        fr.explicitFrames = 100.f;
        renderPresetDirect(fr, W, H, cR, cG, cB, cA);
        lit = 0;
        for (size_t i = 0; i < cA.size(); i++) if (cA[i] > 0.f) lit++;
        printf("    图层生长(满帧): 内容像素=%zu/%zu\n", lit, (size_t)W*H);
        CHECK(lit > (size_t)W*H*3/4, "图层生长满帧覆盖大部 (BFS 从白盘扩散)");
        CHECK(std::fabs(cA[(size_t)16*W+16] - 1.0f) < 0.05f, "图层生长中心覆盖率 = 1 (全场填充)");

        // 7c. 点生长 (默认): 无 layerPts 时默认 {45,45} — 大画布验证默认点行为
        {
            const int W2 = 96, H2 = 96;
            std::vector<float> cR2, cG2, cB2, cA2;
            fr.growthSource = 0;
            fr.splatRadius = 5.f;
            fr.explicitFrames = 10.f;
            fr.layerPts = nullptr;
            fr.layerThresh = nullptr;
            renderPresetDirect(fr, W2, H2, cR2, cG2, cB2, cA2);
            size_t lit = 0, near45 = 0;
            for (size_t i = 0; i < cA2.size(); i++) if (cA2[i] > 0.f) lit++;
            for (int y = 0; y < H2; y++)
                for (int x = 0; x < W2; x++)
                    if (cA2[(size_t)y*W2+x] > 0.f &&
                        std::max(std::abs(x-45), std::abs(y-45)) <= 16)
                        near45++;
            printf("    点生长(默认{45,45}): 内容=%zu 近点=%zu\n", lit, near45);
            CHECK(lit > 0, "点生长有内容 (默认点 {45,45})");
            CHECK(near45 > lit / 2, "内容集中在默认点 {45,45} 附近");
        }
    }

    // 8. Fill_GPU 9 步链 (0x16000): 模糊(2/3) → gamma(4) → speedOverlay(5) → borderControl(9)
    printf("[8] Fill_GPU 9 步链:\n");
    {
        const int W = 32, H = 32;
        Preset pres;
        pres.name = "fg-test";
        pres.duration = 1.f; pres.repeat = 1; pres.compOverOriginal = 0;
        pres.nLayers = 1;
        pres.layers[0].mode = 1;
        pres.layers[0].start = 0.f; pres.layers[0].end = 99.f;
        pres.layers[0].color[0] = 1.f; pres.layers[0].color[1] = 1.f;
        pres.layers[0].color[2] = 1.f; pres.layers[0].color[3] = 1.f;
        pres.layers[0].overlayOpacity = 100.f;
        float pts[2] = { 16.f, 16.f };
        float th[1] = { 0.f };

        // 8a. gamma/exposure (0x33080): fill 0.5 → pow(0.5,2)*1 = 0.25
        DirectFrame fr;
        fr.preset = &pres;
        fr.progress01 = 0.5f;
        fr.explicitFrames = 100.f;
        fr.splatRadius = 8.f;
        fr.growthSource = 0;
        fr.layerPts = pts; fr.layerThresh = th;
        fr.gammaF = 2.f; fr.exposureF = 1.f;
        std::vector<float> cR, cG, cB, cA;
        renderPresetDirect(fr, W, H, cR, cG, cB, cA);
        // 中心 fill≈1 → gamma 后仍 1; 取 fill≈0.5 半径处校验
        printf("    gamma=2: 中心 alpha=%.2f\n", cA[(size_t)16*W+16]);
        CHECK(cA[(size_t)16*W+16] > 0.9f, "gamma=2 中心 fill=1 不变");

        // 8b. borderControl (0x30170): edge=1 处不变, edge=0 处乘 (1-w)
        std::vector<float> edge((size_t)W*H, 1.f);
        edge[(size_t)16*W+16] = 0.f;
        fr.gammaF = 1.f;
        fr.edgeMap = edge.data();
        fr.borderInfluenceF = 0.5f;
        renderPresetDirect(fr, W, H, cR, cG, cB, cA);
        float aC = cA[(size_t)16*W+16];
        printf("    border w=0.5, edge=0: 中心 alpha=%.2f (期望≈0.5)\n", aC);
        CHECK(aC > 0.25f && aC < 0.75f, "borderControl: dst·(1−0.5·(1−0)) = dst/2");

        // 8c. 模糊 (0x24F50): 仅 splat 圆盘 (explicitFrames=0, 无膨胀), 半径 3 后
        //     中心仍满、圆盘边缘外 1px 出现软边 (sigma=0.3R 极窄核, 远处仍为 0)
        fr.edgeMap = nullptr;
        fr.borderInfluenceF = 0.f;
        fr.explicitFrames = 0.f;   // 只留 splat 圆盘 (半径 8, 软边 1px)
        fr.blurRadius = 3.f;
        renderPresetDirect(fr, W, H, cR, cG, cB, cA);
        float bC = cA[(size_t)16*W+16];
        float bFar = cA[(size_t)16*W+7];   // 距中心 9px = 圆盘边缘外 1px
        printf("    blur=3: 中心=%.2f 边缘外1px=%.3f\n", bC, bFar);
        CHECK(bC > 0.9f, "模糊后填充中心保持");
        CHECK(bFar > 0.05f && bFar < 0.9f, "模糊在圆盘边缘外产生软边过渡");
    }

    // 9. BFS 距离场缓存 (staticKey): 同键两次渲染一致; 激活掩码变化触发重算
    printf("[9] BFS 距离场缓存:\n");
    {
        const int W = 32, H = 32;
        Preset pres;
        pres.name = "cache-test";
        pres.duration = 1.f; pres.repeat = 1; pres.compOverOriginal = 0;
        pres.nLayers = 1;
        pres.layers[0].mode = 1;
        pres.layers[0].start = 0.f; pres.layers[0].end = 99.f;
        pres.layers[0].color[0] = 1.f; pres.layers[0].color[1] = 1.f;
        pres.layers[0].color[2] = 1.f; pres.layers[0].color[3] = 1.f;
        pres.layers[0].overlayOpacity = 100.f;
        float pts[2] = { 16.f, 16.f };
        float th0[1] = { 0.f };
        DirectFrame fr;
        fr.preset = &pres;
        fr.progress01 = 0.5f;
        fr.explicitFrames = 20.f;
        fr.splatRadius = 5.f;
        fr.growthSource = 0;
        fr.layerPts = pts; fr.layerThresh = th0;
        fr.staticKey = 0x123456789ULL;   // 启用缓存
        std::vector<float> cR1, cG1, cB1, cA1, cR2, cG2, cB2, cA2;
        renderPresetDirect(fr, W, H, cR1, cG1, cB1, cA1);
        renderPresetDirect(fr, W, H, cR2, cG2, cB2, cA2);
        CHECK(cR1 == cR2 && cA1 == cA2, "同 staticKey 两次渲染逐位一致 (缓存命中)");
        // 掩码变化: 阈值 0.6 > p01=0.5 → 层不激活 → 全空 (不同掩码键)
        float th2[1] = { 0.6f };
        fr.layerThresh = th2;
        std::vector<float> cR3, cG3, cB3, cA3;
        renderPresetDirect(fr, W, H, cR3, cG3, cB3, cA3);
        bool allZero = true;
        for (size_t i = 0; i < cA3.size(); i++) if (cA3[i] > 0.f) { allZero = false; break; }
        CHECK(allZero, "激活掩码变化 (阈值 0.6>p01) 重算后全空");
    }

    // 10. 种子在 alpha 形状外 (原版 Borders 语义回归, 2026-08-17):
    //     点 (4,4) 远离形状 (48..60 方块) → 传播被 Alpha 边界阻断
    //     (原版: "填充不能穿越过透明区域"; 点应置于不透明区域才生效)
    printf("[10] 种子在形状外 (边界阻断):\n");
    {
        const int W = 64, H = 64;
        Preset pres;
        pres.name = "outside-seed";
        pres.duration = 1.f; pres.repeat = 1; pres.compOverOriginal = 0;
        pres.nLayers = 1;
        pres.layers[0].mode = 1;
        pres.layers[0].start = 0.f; pres.layers[0].end = 99.f;
        pres.layers[0].color[0] = 1.f; pres.layers[0].color[1] = 1.f;
        pres.layers[0].color[2] = 1.f; pres.layers[0].color[3] = 1.f;
        pres.layers[0].overlayOpacity = 100.f;
        float pts[2] = { 4.f, 4.f };
        float th0[1] = { 0.f };
        std::vector<float> shape((size_t)W*H, 0.f);
        for (int y = 48; y < 60; y++)
            for (int x = 48; x < 60; x++)
                shape[(size_t)y*W+x] = 1.f;
        DirectFrame fr;
        fr.preset = &pres;
        fr.progress01 = 0.99f;   // 窗口内 (0-99 层 hi=0.99)
        fr.explicitFrames = 100.f;
        fr.splatRadius = 5.f;
        fr.growthSource = 0;
        fr.layerPts = pts; fr.layerThresh = th0;
        fr.shapeAlpha = shape.data();
        std::vector<float> cR, cG, cB, cA;
        renderPresetDirect(fr, W, H, cR, cG, cB, cA);
        float inShape  = cA[(size_t)50*W+50];
        float nearSeed = cA[(size_t)4*W+4];   // 种子处 (形状外)
        printf("    形状内=%.2f 种子处(形状外)=%.2f\n", inShape, nearSeed);
        CHECK(nearSeed < 0.01f, "种子处(形状外)不填充 (splat 显示被 outside 裁剪)");
        CHECK(inShape < 0.01f, "满进度形状内仍为空 (波前被 Alpha 边界阻断)");
    }

    // 11. 层顺序(order)+窗口(start/end)+mode-3 源层 (2026-08-16 "黑底"修复回归):
    //     预设 0 (2 Color Stripes): 蓝(order0)→源(order1)→橙(order3 顶层, 窗口30-60)
    //     白字内容 (用户场景): 45% → 橙条纹压在字上; 80% → 源层盖回白字 (原版 shader 语义),
    //     蓝色只留在透明背景 (字间空隙)
    printf("[11] order/窗口/mode-3 合成:\n");
    {
        const int W = 64, H = 64;
        Preset pres = kPresets[0];  // 2 Color Stripes
        std::vector<float> src((size_t)W*H*4, 0.f);
        std::vector<float> shape((size_t)W*H, 0.f);
        for (int y = 8; y < 56; y++)
            for (int x = 8; x < 56; x++) {
                size_t i = (size_t)y*W+x;
                src[i*4+0] = 1.f; src[i*4+1] = 1.f; src[i*4+2] = 1.f; src[i*4+3] = 1.f;  // 白字
                shape[i] = 1.f;
            }
        float pts[2] = { 10.f, 32.f };   // 种子靠左: 透明背景 (6,32) 在波内
        float th0[1] = { 0.f };
        auto runAt = [&](float p01, size_t px, float* r, float* g, float* b) {
            DirectFrame fr;
            fr.preset = &pres;
            fr.progress01 = p01;
            fr.totalFrames = 30.f;
            fr.explicitFrames = p01 * 30.f;
            fr.splatRadius = 5.f;
            fr.growthSource = 0;
            fr.srcRGBA = src.data();
            fr.shapeAlpha = shape.data();
            fr.layerPts = pts; fr.layerThresh = th0;
            fr.blendMode = 1;
            std::vector<float> cR, cG, cB, cA;
            renderPresetDirect(fr, W, H, cR, cG, cB, cA);
            float a = cA[px], srcA = shape[px];
            bool compOver = (pres.compOverOriginal == 0);
            if (compOver) {
                *r = src[px*4+0]*(1-a) + cR[px]*a;
                *g = src[px*4+1]*(1-a) + cG[px]*a;
                *b = src[px*4+2]*(1-a) + cB[px]*a;
            } else { *r = cR[px]*a; *g = cG[px]*a; *b = cB[px]*a; }
        };
        float r, g, b;
        runAt(0.45f, (size_t)32*W+20, &r, &g, &b);   // 白字像素 (波内, 距种子 10px)
        printf("    45%% 字中心: (%.2f,%.2f,%.2f) 期望橙(1,0.76,0)\n", r, g, b);
        CHECK(r > 0.9f && g > 0.5f && g < 0.9f && b < 0.1f, "45%: 橙条纹顶层可见 (order 排序+窗口)");
        runAt(0.80f, (size_t)32*W+20, &r, &g, &b);   // 白字像素
        printf("    80%% 字中心: (%.2f,%.2f,%.2f) 期望白(源层盖回)\n", r, g, b);
        CHECK(r > 0.9f && g > 0.9f && b > 0.9f, "80%: 源层盖回白字 (原版 shader mode-3 语义)");
        runAt(0.80f, (size_t)32*W+6, &r, &g, &b);   // 形状外透明背景 (x=6 < 8)
        printf("    80%% 形状外背景: (%.2f,%.2f,%.2f) 期望透明黑(边界阻断, 不填充)\n", r, g, b);
        CHECK(r < 0.05f && g < 0.05f && b < 0.05f, "80%: 形状外保持透明 (原版边界阻断)");
    }

    // 12. 传播掩码 (propMask, 原版 Borders/Bridges) + Speed 整步 [2026-08-18]
    //     双竖条形状 (左 8px / 右 8px, 中 16px 空隙): 无桥接波前被空隙阻断;
    //     膨胀掩码 (桥接) → 波前越过空隙达右条。
    printf("[12] 传播掩码 + Speed 整步:\n");
    {
        const int W = 32, H = 32;
        Preset pres;
        pres.name = "pm-test";
        pres.duration = 1.f; pres.repeat = 1; pres.compOverOriginal = 0;
        pres.nLayers = 1;
        pres.layers[0].mode = 1;
        pres.layers[0].start = 0.f; pres.layers[0].end = 99.f;
        pres.layers[0].color[0] = 1.f; pres.layers[0].color[1] = 1.f;
        pres.layers[0].color[2] = 1.f; pres.layers[0].color[3] = 1.f;
        pres.layers[0].overlayOpacity = 100.f;
        std::vector<float> shape((size_t)W*H, 0.f);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (x < 8 || x >= 24) shape[(size_t)y*W+x] = 1.f;
        float pts[2] = { 4.f, 16.f };   // 种子在左条内
        float th0[1] = { 0.f };
        DirectFrame fr;
        fr.preset = &pres;
        fr.progress01 = 0.99f;
        fr.totalFrames = 100.f;
        fr.explicitFrames = 40.f;
        fr.splatRadius = 4.f;
        fr.growthSource = 0;
        fr.layerPts = pts; fr.layerThresh = th0;
        fr.shapeAlpha = shape.data();
        std::vector<float> cR, cG, cB, cA;
        auto countRight = [&](const std::vector<float>& A) {
            size_t lit = 0;
            for (int y = 0; y < H; y++)
                for (int x = 24; x < W; x++)
                    if (A[(size_t)y*W+x] > 0.f) lit++;
            return lit;
        };
        // 12a. 无桥接: 空隙阻断 → 右条无内容
        renderPresetDirect(fr, W, H, cR, cG, cB, cA);
        size_t rightLit = countRight(cA);
        size_t leftLit = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < 8; x++)
                if (cA[(size_t)y*W+x] > 0.f) leftLit++;
        printf("    无桥接: 左条=%zu 右条=%zu\n", leftLit, rightLit);
        CHECK(rightLit == 0, "空隙阻断: 无桥接时波前不达右条 (Borders)");
        CHECK(leftLit > 0, "左条有生长内容");
        // 12b. propMask = 形状膨胀 16px (桥接/弱边界) → 波前越过空隙达右条
        std::vector<float> prop((size_t)W*H, 0.f);
        dissolve::dilateMaxField(shape, prop, W, H, 16);
        fr.propMask = prop.data();
        fr.explicitFrames = 100.f;  // 满帧: 掩码内全部覆盖
        renderPresetDirect(fr, W, H, cR, cG, cB, cA);
        rightLit = countRight(cA);
        size_t gapLit = 0;
        for (int y = 0; y < H; y++)
            for (int x = 8; x < 24; x++)
                if (cA[(size_t)y*W+x] > 0.f) gapLit++;
        printf("    桥接(膨胀16): 右条=%zu 空隙=%zu\n", rightLit, gapLit);
        CHECK(rightLit > 0, "桥接掩码: 波前越过空隙达右条");
        CHECK(gapLit > 0, "桥接掩码: 空隙区域可见填充 (溢出/桥接显示)");
        // 12c. Speed 整步 (steps per second, 离散模拟步): floor(10.5)==10
        fr.propMask = nullptr;
        fr.explicitFrames = 10.f;
        renderPresetDirect(fr, W, H, cR, cG, cB, cA);
        std::vector<float> cA10 = cA;
        fr.explicitFrames = 10.5f;
        renderPresetDirect(fr, W, H, cR, cG, cB, cA);
        bool same = true;
        for (size_t i = 0; i < cA.size(); i++)
            if (std::fabs(cA[i] - cA10[i]) > 1e-4f) { same = false; break; }
        printf("    整步: 帧10.5 == 帧10: %s\n", same ? "是" : "否");
        CHECK(same, "Speed 整步: floor(10.5)==10 → 结果一致");
        fr.explicitFrames = 11.f;
        renderPresetDirect(fr, W, H, cR, cG, cB, cA);
        bool diff = false;
        for (size_t i = 0; i < cA.size(); i++)
            if (std::fabs(cA[i] - cA10[i]) > 1e-3f) { diff = true; break; }
        printf("    整步: 帧11 != 帧10: %s\n", diff ? "是" : "否");
        CHECK(diff, "Speed 整步: 帧推进改变结果");
    }

    printf("\n%s (%d 失败)\n", failures == 0 ? "== 全部通过 ==" : "== 有失败 ==", failures);
    return failures;
}
