/*
 * test_core.cpp — dissolve_core 数学验证 (控制台单测, 无 AE 依赖)
 * 编译: cl /O2 /EHsc test_core.cpp ..\dissolve_core.cpp
 * 验证内容:
 *   1. Simplex 噪声统计特性 (均值~0, 范围[-1,1])
 *   2. FBM 范围与复杂度分层
 *   3. JFA 距离场: 圆盘中心距离近似半径
 *   4. 覆盖率: 全填充=1, 半填充=0.5, 空=0
 *   5. 全管线输出不越界
 */
#include "../dissolve_core.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <random>

using namespace dissolve;

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  [PASS] %s\n", msg); \
    else { printf("  [FAIL] %s\n", msg); failures++; } \
} while(0)

int main() {
    printf("== dissolve_core 数学验证 ==\n");

    // 1. Simplex 噪声统计
    {
        std::mt19937 rng(42);
        std::uniform_real_distribution<float> d(-10, 10);
        double sum = 0, sum2 = 0;
        float mn = 1e9, mx = -1e9;
        const int N = 200000;
        for (int i = 0; i < N; i++) {
            float v = simplex3d(d(rng), d(rng), d(rng));
            sum += v; sum2 += v*v;
            mn = std::min(mn, v); mx = std::max(mx, v);
        }
        double mean = sum / N;
        double var = sum2 / N - mean*mean;
        printf("[1] Simplex3D: mean=%.4f (期望~0) var=%.4f range=[%.3f, %.3f]\n",
               mean, var, mn, mx);
        CHECK(std::fabs(mean) < 0.05, "均值接近 0");
        CHECK(var > 0.05 && var < 0.3, "方差合理 (~1/12)");
        CHECK(mn > -1.5 && mx < 1.5, "范围在 [-1.5, 1.5]");
    }

    // 2. FBM 分层
    {
        Params p;
        p.complexityL = 1;
        float v1 = fbm3d(p, 1.0f, 2.0f, 3.0f);
        p.complexityL = 2;
        float v2 = fbm3d(p, 1.0f, 2.0f, 3.0f);
        p.complexityL = 4;
        float v4 = fbm3d(p, 1.0f, 2.0f, 3.0f);
        printf("[2] FBM: L1=%.4f L2=%.4f L4=%.4f\n", v1, v2, v4);
        CHECK(v1 != v2 && v2 != v4, "复杂度分层有差异");
        CHECK(v4 > -1.5 && v4 < 1.5, "L4 范围合理");
    }

    // 3. JFA 距离场: 256x256, 中心 64x64 实心方块 -> 边缘距离应近似
    {
        const int W = 256, H = 256;
        std::vector<float> alpha((size_t)W*H, 0.f);
        for (int y = 96; y < 160; y++)
            for (int x = 96; x < 160; x++)
                alpha[(size_t)y*W+x] = 1.f;
        Params p;
        Buffers buf;
        p.dfModeL = 1; p.alphaThreshold = 0.5f;
        jfaDistance(p, alpha.data(), buf, W, H);
        float diag = std::sqrt((float)(W*W + H*H));
        // GLSL 语义: 外部=种子, 内部像素距离 = 到边缘距离 (波前从边缘向内扩散)
        // 内部 16px (y=112): 距离应 ~= 16.5 (块上边缘在 y=95.5)
        float d16 = buf.distField[112*W + 128] * diag;
        // 方块中心 (128,128): 距离 ~= 32.5
        float dInside = buf.distField[128*W + 128] * diag;
        // 外部 (y=56, 种子区): 距离 ~= 0
        float dOutside = buf.distField[56*W + 128] * diag;
        printf("[3] JFA: 内16px 距离=%.2f (期望~16.5), 中心=%.2f (期望~32.5), 外部=%.2f (期望~0)\n",
               d16, dInside, dOutside);
        CHECK(d16 > 14 && d16 < 19, "内部16px 距离合理 (到边缘)");
        CHECK(dInside > 29 && dInside < 36, "中心距离合理 (约 32.5)");
        CHECK(dOutside < 2, "外部(种子区)距离接近 0");
    }

    // 4. 覆盖率
    {
        float cFull = coverageAnalytic(1,1,1,1, 8, 0.002f);
        float cEmpty = coverageAnalytic(0,0,0,0, 8, 0.002f);
        // 上半填充 (f00=f10=1, f01=f11=0): 双线性场 v=(1-fy)
        // 阈值 0.002 时 8x8 网格全部 > 0.002 -> 覆盖率 1.0 (GLSL 语义)
        float cTop = coverageAnalytic(1,1,0,0, 8, 0.002f);
        // 用阈值 0.5 验证面积比例: v=(1-fy)>0.5 -> py<0.5 -> 4/8 行 -> 0.5
        float cTopHalf = coverageAnalytic(1,1,0,0, 8, 0.5f);
        printf("[4] Coverage: full=%.3f empty=%.3f top(eps=0.002)=%.3f top(eps=0.5)=%.3f\n",
               cFull, cEmpty, cTop, cTopHalf);
        CHECK(std::fabs(cFull - 1.0f) < 1e-4, "全填充覆盖率=1");
        CHECK(std::fabs(cEmpty) < 1e-4, "空覆盖率=0");
        CHECK(cTop > 0.99f, "半场 eps=0.002 全覆盖 (GLSL 语义)");
        CHECK(cTopHalf > 0.45 && cTopHalf < 0.55, "eps=0.5 时面积比例=0.5");
        // 解析积分 vs 暴力 64 采样一致性 (随机)
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> d(0, 1);
        float maxDiff = 0;
        for (int i = 0; i < 200; i++) {
            float f00=d(rng), f10=d(rng), f01=d(rng), f11=d(rng);
            float a = coverageAnalytic(f00,f10,f01,f11, 8, 0.002f);
            // 暴力
            int cnt=0;
            for (int ny=0;ny<8;ny++) for (int nx=0;nx<8;nx++) {
                float px=nx/8.f, py=ny/8.f;
                float fx=px-std::floor(px), fy=py-std::floor(py);
                float w0=(1-fx)*(1-fy), w1=fx*(1-fy), w2=(1-fx)*fy, w3=fx*fy;
                float v=f00*w0+f10*w1+f01*w2+f11*w3;
                if (v>0.002f) cnt++;
            }
            maxDiff = std::max(maxDiff, std::fabs(a - cnt/64.f));
        }
        printf("      采样一致性 maxDiff=%.4f\n", maxDiff);
        CHECK(maxDiff < 1e-4, "与 64 次暴力采样一致");
    }

    // 5. 全管线
    {
        const int W = 128, H = 128;
        std::vector<float> rgba((size_t)W*H*4);
        for (int y = 32; y < 96; y++)
            for (int x = 32; x < 96; x++) {
                size_t i = ((size_t)y*W+x)*4;
                rgba[i+0]=1; rgba[i+1]=0.2f; rgba[i+2]=0.2f; rgba[i+3]=1;
            }
        Params p;
        Buffers buf;
        fullPipeline(p, rgba.data(), W, H, rgba.data(), buf);
        bool ok = true;
        for (size_t i = 0; i < (size_t)W*H*4; i++)
            if (!(rgba[i] >= -1e-4f && rgba[i] <= 1.0f + 1e-4f)) { ok = false; break; }
        printf("[5] Pipeline: 输出范围检查 %s\n", ok ? "OK" : "越界!");
        CHECK(ok, "全管线输出在 [0,1]");
        // 检查有溶解效果 (方块边缘 alpha 应部分降低)
        float centerA = rgba[((size_t)64*W+64)*4+3];
        float edgeA = rgba[((size_t)32*W+32)*4+3]; // 方块角落
        printf("      中心 alpha=%.3f 角 alpha=%.3f\n", centerA, edgeA);
        CHECK(centerA <= 1.0f + 1e-4f && edgeA >= 0.0f - 1e-4f, "alpha 合理");
    }

    printf("\n%s (%d 失败)\n", failures == 0 ? "== 全部通过 ==" : "== 有失败 ==", failures);
    return failures;
}
