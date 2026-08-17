/* growth_sim.cpp — 跨帧 growthStep 模拟 vs BFS 单帧近似对比
 * 目的: 验证差异 1 (跨帧双时间戳传播 vs BFS 距离场) 的实际形状差异,
 *       为"是否把 growthStep 接入 renderPreset"提供数据
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../dissolve_core.h"
#include "../dissolve_styles.h"
#include <cstdio>
#include <vector>
#include <cmath>

using namespace dissolve;

int main() {
    const int W = 160, H = 160;
    // 圆形 alpha 形状 (同 growth_regress)
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

    // 种子 = 中心 (最深点)
    int sx = 80, sy = 80;

    // ---- A) 跨帧 growthStep: 首帧 fill = 种子圆 (半径 3), 每帧 +1px ----
    // 模拟原版: fillTex 初始 = 种子掩码, Growth 每帧用 8x8 覆盖率推进
    // growthStep 的 fill 输入是 fillMap (buf.fillMap) — 用当前 fill 场
    std::vector<float> time1((size_t)W*H, 0.f), cov1((size_t)W*H, 0.f);
    std::vector<float> time2((size_t)W*H, 0.f), cov2((size_t)W*H, 0.f);
    // 首帧 fill 场 = 种子圆 (半径 3, 硬边)
    buf.fillMap.assign((size_t)W*H, 0.f);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float dx = (float)(x - sx), dy = (float)(y - sy);
            if (dx*dx + dy*dy <= 9.f && alpha[(size_t)y*W+x] > 0.5f)
                buf.fillMap[(size_t)y*W+x] = 1.f;
        }
    p.timeF = 1.f;
    p.numSamples = 8;
    printf("== 跨帧 growthStep 模拟 (fill 每帧 dilate 1px, 原版 0x144801) ==\n");
    float covAt = 0.f;
    std::vector<float> covFrames;
    for (int frame = 0; frame <= 120; frame++) {
        growthStep(p, buf, W, H,
                   frame == 0 ? nullptr : time1.data(),
                   frame == 0 ? nullptr : cov1.data(),
                   frame == 0 ? nullptr : time2.data(),
                   frame == 0 ? nullptr : cov2.data(),
                   time1.data(), cov1.data(), time2.data(), cov2.data());
        // dilate: fill 场 = 3x3 max (扩张 1px), 形状内 (alpha>0.5) 保留
        std::vector<float> nd2((size_t)W*H, 0.f);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                if (alpha[(size_t)y*W+x] <= 0.5f) continue;
                float mx = 0.f;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int px = x + dx, py = y + dy;
                        if (px < 0 || py < 0 || px >= W || py >= H) continue;
                        float v = buf.fillMap[(size_t)py*W+px];
                        if (v > mx) mx = v;
                    }
                nd2[(size_t)y*W+x] = mx;
            }
        buf.fillMap = nd2;
        size_t lit = 0;
        for (size_t i = 0; i < (size_t)W*H; i++)
            if (alpha[i] > 0.5f && buf.fillMap[i] > 0.5f) lit++;
        covAt = (float)lit / (float)inShape;
        if (frame % 10 == 0 || frame == 120)
            printf("  帧 %3d: 覆盖比例 %.3f\n", frame, covAt);
        if (frame == 40) covFrames.push_back(covAt);
        if (frame == 80) covFrames.push_back(covAt);
    }
    printf("  growthStep 40帧=%.3f 80帧=%.3f\n", covFrames[0], covFrames[1]);

    // ---- B) BFS 单帧近似: fill = 1-smoothstep(p01, p01+SOFT, nd) ----
    // BFS 从中心传播 (复刻 renderPreset 逻辑)
    std::vector<float> bfs((size_t)W*H, 1e9f);
    std::vector<int> qx, qy;
    bfs[(size_t)sy*W+sx] = 0.f; qx.push_back(sx); qy.push_back(sy);
    const int dx8[8] = {1,-1,0,0,1,1,-1,-1}, dy8[8] = {0,0,1,-1,1,-1,1,-1};
    float maxB = 1.f;
    for (size_t qi = 0; qi < qx.size(); qi++) {
        int x = qx[qi], y = qy[qi];
        float d = bfs[(size_t)y*W+x];
        if (d > maxB) maxB = d;
        for (int k = 0; k < 8; k++) {
            int nx = x+dx8[k], ny = y+dy8[k];
            if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
            size_t ni = (size_t)ny*W+nx;
            if (bfs[ni] > d + 1.f) {
                if (alpha[ni] <= 0.05f) continue;
                bfs[ni] = d + 1.f; qx.push_back(nx); qy.push_back(ny);
            }
        }
    }
    printf("\n== BFS 单帧近似 (smoothstep SOFT=0.05) ==\n");
    float b40 = 0, b80 = 0;
    for (float prog : {33.3f, 66.6f}) {
        float p01 = prog / 100.f;
        size_t lit = 0;
        for (size_t i = 0; i < (size_t)W*H; i++) {
            if (alpha[i] <= 0.5f) continue;
            float nd = std::min(bfs[i] / maxB, 1.f);
            float f = 1.f - smoothstepField(p01, p01 + 0.05f, nd);
            if (f > 0.05f) lit++;
        }
        float r = (float)lit / (float)inShape;
        printf("  prog=%.1f%% (33%%/66%%≈40/80帧): 覆盖比例 %.3f\n", prog, r);
        if (prog < 50) b40 = r; else b80 = r;
    }
    printf("  BFS 33%%=%.3f 66%%=%.3f (与 growthStep 40帧/80帧对照)\n", b40, b80);

    // ---- C) BFS 波前形状 vs growthStep 波前形状 (边缘距离分布) ----
    printf("\n== 波前形状对比 (50%% 覆盖时的边缘距离) ==\n");
    // growthStep 80 帧时的 cov1 边缘: 统计 cov1 0.01-0.99 像素的 BFS 距离范围
    float lo = 1e9f, hi = -1e9f;
    for (size_t i = 0; i < (size_t)W*H; i++) {
        if (alpha[i] <= 0.5f) continue;
        if (cov1[i] > 0.01f && cov1[i] < 0.99f && time1[i] > 0.f) {
            float nd = std::min(bfs[i] / maxB, 1.f);
            if (nd < lo) lo = nd;
            if (nd > hi) hi = nd;
        }
    }
    printf("  growthStep 120帧波前边缘: nd ∈ [%.3f, %.3f] (宽度 %.3f)\n", lo, hi, hi - lo);
    printf("  (BFS smoothstep SOFT=0.05 软边宽度固定 0.05)\n");
    return 0;
}
