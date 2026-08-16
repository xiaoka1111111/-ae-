/*
 * test_gpu.cpp — GPU 渲染器验证
 * 编译: cl /O2 /EHsc test_gpu.cpp ..\gl_renderer.cpp ..\dissolve_core.cpp ..\dissolve_styles.cpp
 */
#include "../gl_renderer.h"
#include "../dissolve_core.h"
#include "../dissolve_styles.h"
#include "../preset_data.h"
#include <cstdio>
#include <cmath>
#include <vector>

using namespace dissolve;

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  [PASS] %s\n", msg); \
    else { printf("  [FAIL] %s\n", msg); failures++; } \
} while(0)

int main() {
    printf("== GPU 渲染器验证 ==\n");

    // 1. GL 上下文
    printf("[1] OpenGL 初始化:\n");
    bool ok = glr::init();
    CHECK(ok, "GL 3.3 上下文创建成功");
    if (!ok) { printf("  跳过后续测试 (无 GL 环境)\n"); return 1; }
    CHECK(glr::available(), "available() 报告可用");

    // 2. 全预设 GPU 渲染
    printf("[2] GPU 渲染 30 预设:\n");
    fflush(stdout);
    const int W = 96, H = 96;
    std::vector<float> src((size_t)W*H*4, 0.f);
    for (int y = 24; y < 72; y++)
        for (int x = 24; x < 72; x++) {
            size_t i = ((size_t)y*W+x)*4;
            src[i+0]=0.2f; src[i+1]=0.4f; src[i+2]=0.6f; src[i+3]=1.f;
        }
    float noiseParams[7] = {1.f, 1.f, 1.f, 0.5f, 1.f, 0.f, 1.f};
    printf("  -> 单帧测试 (Fire @50%%):\n"); fflush(stdout);
    {
        std::vector<float> out;
        bool r = glr::renderFrame(src.data(), W, H, kPresets[7], 50.f,
                                  noiseParams, 4, 0.5f, 0.5f, 0.f, 1.f, 1.f, 0, out);
        printf("  renderFrame 返回: %d, out.size=%zu\n", r, out.size()); fflush(stdout);
        if (r && !out.empty()) {
            printf("  中心像素: (%.3f, %.3f, %.3f, %.3f)\n",
                   out[((size_t)48*W+48)*4+0], out[((size_t)48*W+48)*4+1],
                   out[((size_t)48*W+48)*4+2], out[((size_t)48*W+48)*4+3]);
        }
    }
    printf("  -> 全部预设:\n"); fflush(stdout);
    int badRange = 0, gpuFail = 0;
    for (int pi = 0; pi < kNumPresets; pi++) {
        std::vector<float> out;
        bool r = glr::renderFrame(src.data(), W, H, kPresets[pi], 50.f,
                                  noiseParams, 4, 0.5f, 0.5f, 0.f, 1.f, 1.f, 0, out);
        if (!r) { gpuFail++; continue; }
        for (size_t i = 0; i < out.size(); i++)
            if (!(out[i] >= -1e-3f && out[i] <= 1.001f)) { badRange++; break; }
    }
    CHECK(gpuFail == 0, "30 预设 GPU 渲染全部成功");
    CHECK(badRange == 0, "GPU 输出全部在 [0,1]");

    // 3. CPU vs GPU 对比 (Fire 预设, 50%)
    printf("[3] CPU vs GPU 一致性 (Fire @50%%):\n");
    {
        Params p;
        Buffers buf;
        std::vector<float> alpha((size_t)W*H);
        for (size_t i = 0; i < (size_t)W*H; i++) alpha[i] = src[i*4+3];
        generateNoiseMap(p, buf, W, H);
        jfaDistance(p, alpha.data(), buf, W, H);
        StyleFrame fr;
        fr.preset = &kPresets[7];  // Fire
        fr.progress = 50.f;
        fr.params = &p;  // Fill_GPU: 与 GPU 路径一致 (speed=0.5 默认)
        std::vector<float> cR, cG, cB, cA;
        renderPreset(fr, buf, src.data(), W, H, cR, cG, cB, cA);

        std::vector<float> gout;
        glr::renderFrame(src.data(), W, H, kPresets[7], 50.f,
                         noiseParams, 4, 0.5f, 0.5f, 0.f, 1.f, 1.f, 0, gout);
        // 对比中心区域 (形状内)
        double diff = 0; int cnt = 0;
        for (int y = 28; y < 68; y++)
            for (int x = 28; x < 68; x++) {
                size_t i = (size_t)y*W+x;
                diff += std::fabs(cA[i] - gout[i*4+3]);
                diff += std::fabs(cR[i] - gout[i*4+0]);
                cnt += 2;
            }
        diff /= cnt;
        printf("    GPU 中心 alpha=%.3f, CPU alpha=%.3f, 平均差=%.4f\n",
               gout[((size_t)48*W+48)*4+3], cA[(size_t)48*W+48], diff);
        printf("    GPU 中心 RGB=(%.3f,%.3f,%.3f) CPU 中心 RGB=(%.3f,%.3f,%.3f) src=(0.2,0.4,0.6)\n",
               gout[((size_t)48*W+48)*4+0], gout[((size_t)48*W+48)*4+1], gout[((size_t)48*W+48)*4+2],
               cR[(size_t)48*W+48], cG[(size_t)48*W+48], cB[(size_t)48*W+48]);
        // GPU 用对角线近似归一化波前, CPU 用精确 maxDist — 数值有差异但语义一致:
        // 两者中心 alpha 都饱和, 且颜色方向一致 (Fire: 红色系)
        // 波前从边缘向内推进: 中心 wave=1 需 tt=1 才激活; 50% 时两者应一致 (均未激活或均饱和)
        CHECK(std::fabs(gout[((size_t)48*W+48)*4+3] - cA[(size_t)48*W+48]) < 0.15f,
              "GPU 与 CPU 中心 alpha 一致 (波前推进同步)");
        // Fire 中心 = mode=3 原图显现层; 波前从边缘向内部推进: 50% 时中心(wave=1)尚未到达
        // GPU 输出是预乘色 (RGB 已乘 alpha), CPU 输出是纯层色+独立 alpha — 比较 cR*cA
        {
            size_t ci = (size_t)48*W+48;
            float pr = cR[ci]*cA[ci], pg = cG[ci]*cA[ci], pb = cB[ci]*cA[ci];
            CHECK(std::fabs(gout[ci*4+0] - pr) < 0.15f &&
                  std::fabs(gout[ci*4+1] - pg) < 0.15f &&
                  std::fabs(gout[ci*4+2] - pb) < 0.15f &&
                  std::fabs(gout[ci*4+3] - cA[ci]) < 0.15f,
                  "GPU 输出 = CPU 层色预乘 alpha (中心未激活或部分激活)");
        }
    }

    // 4. 多进度 GPU 渲染稳定
    printf("[4] 多进度渲染:\n");
    bool stable = true;
    for (float prog : {0.f, 25.f, 50.f, 75.f, 100.f}) {
        std::vector<float> out;
        if (!glr::renderFrame(src.data(), W, H, kPresets[4], prog,
                              noiseParams, 4, 0.5f, 0.5f, 0.f, 1.f, 1.f, 0, out)) stable = false;
    }
    CHECK(stable, "Glow 预设 5 个进度点渲染稳定");

    // 5. mode=3 原图显现层 GPU 采样验证 (Soft Shadow @50%: 波前从中心种子向外)
    printf("[5] mode=3 源图采样 (Soft Shadow @50%%):\n");
    {
        // 用 CPU 参考输出对比 (设计语义: 种子在形状内部, 波前向外推进)
        Params p2;
        Buffers buf2;
        std::vector<float> alpha2((size_t)W*H);
        for (size_t i = 0; i < (size_t)W*H; i++) alpha2[i] = src[i*4+3];
        generateNoiseMap(p2, buf2, W, H);
        jfaDistance(p2, alpha2.data(), buf2, W, H);
        StyleFrame fr2;
        fr2.preset = &kPresets[29];  // Soft Shadow
        fr2.progress = 50.f;
        fr2.params = &p2;  // Fill_GPU 与 GPU 一致
        std::vector<float> cR2, cG2, cB2, cA2;
        renderPreset(fr2, buf2, src.data(), W, H, cR2, cG2, cB2, cA2);

        std::vector<float> gout;
        glr::renderFrame(src.data(), W, H, kPresets[29], 50.f,
                         noiseParams, 4, 0.5f, 0.5f, 0.f, 1.f, 1.f, 0, gout);
        size_t ci = (size_t)48*W+48;
        // GPU 输出为预乘色: 比较 GPU vs CPU 层色预乘 alpha
        CHECK(std::fabs(gout[ci*4+3] - cA2[ci]) < 0.1f,
              "GPU 中心 alpha 与 CPU 一致 (波前推进同步)");
        CHECK(std::fabs(gout[ci*4+0] - cR2[ci]*cA2[ci]) < 0.15f &&
              std::fabs(gout[ci*4+1] - cG2[ci]*cA2[ci]) < 0.15f &&
              std::fabs(gout[ci*4+2] - cB2[ci]*cA2[ci]) < 0.15f,
              "GPU 中心输出 = CPU 层色预乘 (覆盖率区间语义一致)");
    }

    // 6. blur 层 CPU/GPU 对齐 (Fire Wave @50%: 层 blur=223, 权重模糊敏感)
    printf("[6] blur 层 CPU/GPU 对齐 (Fire Wave @50%%):\n");
    {
        Params p3;
        Buffers buf3;
        std::vector<float> alpha3((size_t)W*H);
        for (size_t i = 0; i < (size_t)W*H; i++) alpha3[i] = src[i*4+3];
        generateNoiseMap(p3, buf3, W, H);
        jfaDistance(p3, alpha3.data(), buf3, W, H);
        StyleFrame fr3;
        fr3.preset = &kPresets[15];  // Fire Wave
        fr3.progress = 50.f;
        fr3.params = &p3;  // Fill_GPU 与 GPU 一致
        std::vector<float> cR3, cG3, cB3, cA3;
        renderPreset(fr3, buf3, src.data(), W, H, cR3, cG3, cB3, cA3);

        std::vector<float> gout3;
        bool r3 = glr::renderFrame(src.data(), W, H, kPresets[15], 50.f,
                                   noiseParams, 4, 0.5f, 0.5f, 0.f, 1.f, 1.f, 0, gout3);
        CHECK(r3, "Fire Wave GPU 渲染成功");
        if (r3) {
            double diff3 = 0; int cnt3 = 0;
            double diffA = 0; int cntA = 0;
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    size_t i = (size_t)y*W+x;
                    diff3 += std::fabs(cA3[i] - gout3[i*4+3]);
                    cnt3++;
                    if (alpha3[i] > 0.5f) {  // 形状内 alpha 差
                        diffA += std::fabs(cA3[i] - gout3[i*4+3]);
                        cntA++;
                    }
                }
            diff3 /= cnt3; diffA /= cntA;
            printf("    Fire Wave 全图平均差=%.4f, 形状内 alpha 平均差=%.4f\n", diff3, diffA);
            CHECK(diffA < 0.12f, "形状内 alpha 平均差 < 0.12 (blur 权重对齐)");
            CHECK(diff3 < 0.05f, "全图平均差 < 0.05 (形状外干净)");
        }
    }

    glr::shutdown();
    printf("\n%s (%d 失败)\n", failures == 0 ? "== 全部通过 ==" : "== 有失败 ==", failures);
    return failures;
}


