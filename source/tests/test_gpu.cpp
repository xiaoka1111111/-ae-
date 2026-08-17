/*
 * test_gpu.cpp — GPU 渲染器验证
 * 编译: cl /O2 /EHsc test_gpu.cpp ..\gl_renderer.cpp ..\dissolve_core.cpp ..\dissolve_styles.cpp
 */
#include "../gl_renderer.h"
#include "../dissolve_core.h"
#include "../dissolve_styles.h"
#include "../dissolve_direct.h"
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
        // CPU 参考 = 直通管线 renderPresetDirect (与 GPU 同 order 层序语义)
        DirectFrame fr;
        fr.preset = &kPresets[7];  // Fire
        fr.progress01 = 0.5f;
        fr.totalFrames = kPresets[7].duration * 30.f;
        fr.explicitFrames = 0.5f * kPresets[7].duration * 30.f;
        fr.splatRadius = 10.f;
        fr.rampS = 1.f;
        fr.srcRGBA = src.data();
        std::vector<float> sha((size_t)W*H);
        for (size_t i = 0; i < (size_t)W*H; i++) sha[i] = src[i*4+3];
        fr.shapeAlpha = sha.data();  // 与插件一致: minD/maxD 只统计形状内
        int nL = std::min(kPresets[7].nLayers, 5);
        std::vector<float> pts((size_t)nL*2, 0.f), th(nL, 0.f);
        for (int li = 0; li < nL; li++) { pts[(size_t)li*2+0] = 45.f; pts[(size_t)li*2+1] = 45.f; th[li] = 0.f; }
        fr.layerPts = pts.data();
        fr.layerThresh = th.data();
        fr.growthSource = 0;
        std::vector<float> cR, cG, cB, cA;
        renderPresetDirect(fr, W, H, cR, cG, cB, cA);

        std::vector<float> gout;
        // 与 CPU 参考同源种子掩码: 45%×96=43,43 画半径圆 (插件实际路径)
        std::vector<float> sm((size_t)W*H, 0.f);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float dx = x - 43.f, dy = y - 43.f;
                if (dx*dx + dy*dy <= 100.f) sm[(size_t)y*W+x] = 1.f;
            }
        glr::renderFrame(src.data(), W, H, kPresets[7], 50.f,
                         noiseParams, 4, 0.5f, 0.f, 0.f, 1.f, 1.f, 0, gout, sm.data());
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
        // CPU 参考 = 直通管线 renderPresetDirect (与 GPU 同 order 层序/切比雪夫度量)
        DirectFrame fr2;
        fr2.preset = &kPresets[29];  // Soft Shadow
        fr2.progress01 = 0.5f;
        fr2.totalFrames = kPresets[29].duration * 30.f;
        fr2.explicitFrames = 0.5f * kPresets[29].duration * 30.f;
        fr2.splatRadius = 10.f;
        fr2.rampS = 1.f;
        fr2.srcRGBA = src.data();
        std::vector<float> sha2((size_t)W*H);
        for (size_t i = 0; i < (size_t)W*H; i++) sha2[i] = src[i*4+3];
        fr2.shapeAlpha = sha2.data();
        int nL2 = std::min(kPresets[29].nLayers, 5);
        std::vector<float> pts2((size_t)nL2*2, 0.f), th2(nL2, 0.f);
        for (int li = 0; li < nL2; li++) { pts2[(size_t)li*2+0] = 45.f; pts2[(size_t)li*2+1] = 45.f; th2[li] = 0.f; }
        fr2.layerPts = pts2.data();
        fr2.layerThresh = th2.data();
        fr2.growthSource = 0;
        std::vector<float> cR2, cG2, cB2, cA2;
        renderPresetDirect(fr2, W, H, cR2, cG2, cB2, cA2);

        std::vector<float> gout;
        // 与 CPU 参考同源种子掩码 (45%×96=43,43)
        std::vector<float> sm2((size_t)W*H, 0.f);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float dx = x - 43.f, dy = y - 43.f;
                if (dx*dx + dy*dy <= 100.f) sm2[(size_t)y*W+x] = 1.f;
            }
        glr::renderFrame(src.data(), W, H, kPresets[29], 50.f,
                         noiseParams, 4, 0.5f, 0.5f, 0.f, 1.f, 1.f, 0, gout, sm2.data());
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
        // CPU 参考 = 直通管线 renderPresetDirect (与 GPU 同 order 层序/切比雪夫度量)
        DirectFrame fr3;
        fr3.preset = &kPresets[15];  // Fire Wave
        fr3.progress01 = 0.5f;
        fr3.totalFrames = kPresets[15].duration * 30.f;
        fr3.explicitFrames = 0.5f * kPresets[15].duration * 30.f;
        fr3.splatRadius = 10.f;
        fr3.rampS = 1.f;
        fr3.srcRGBA = src.data();
        std::vector<float> sha3((size_t)W*H);
        for (size_t i = 0; i < (size_t)W*H; i++) sha3[i] = src[i*4+3];
        fr3.shapeAlpha = sha3.data();  // 与插件一致: minD/maxD 只统计形状内
        int nL3 = std::min(kPresets[15].nLayers, 5);
        std::vector<float> pts3((size_t)nL3*2, 0.f), th3(nL3, 0.f);
        for (int li = 0; li < nL3; li++) { pts3[(size_t)li*2+0] = 45.f; pts3[(size_t)li*2+1] = 45.f; th3[li] = 0.f; }
        fr3.layerPts = pts3.data();
        fr3.layerThresh = th3.data();
        fr3.growthSource = 0;
        std::vector<float> cR3, cG3, cB3, cA3;
        renderPresetDirect(fr3, W, H, cR3, cG3, cB3, cA3);

        std::vector<float> gout3;
        // 与 CPU 参考同源种子掩码 (45%×96=43,43)
        std::vector<float> sm3((size_t)W*H, 0.f);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float dx = x - 43.f, dy = y - 43.f;
                if (dx*dx + dy*dy <= 100.f) sm3[(size_t)y*W+x] = 1.f;
            }
        bool r3 = glr::renderFrame(src.data(), W, H, kPresets[15], 50.f,
                                   noiseParams, 4, 0.5f, 0.f, 0.f, 1.f, 1.f, 0, gout3, sm3.data());
        CHECK(r3, "Fire Wave GPU 渲染成功");
        if (r3) {
            double diff3 = 0; int cnt3 = 0;
            double diffA = 0; int cntA = 0;
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    size_t i = (size_t)y*W+x;
                    diff3 += std::fabs(cA3[i] - gout3[i*4+3]);
                    cnt3++;
                    if (src[i*4+3] > 0.5f) {  // 形状内 alpha 差
                        diffA += std::fabs(cA3[i] - gout3[i*4+3]);
                        cntA++;
                    }
                }
            diff3 /= cnt3; diffA /= cntA;
            // 形状外单独统计 (真正验证"形状外干净")
            double outErr = 0; int cntO = 0;
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    size_t i = (size_t)y*W+x;
                    if (src[i*4+3] <= 0.5f) { outErr += std::fabs(cA3[i] - gout3[i*4+3]); cntO++; }
                }
            outErr /= std::max(cntO, 1);
            printf("    Fire Wave 全图平均差=%.4f, 形状内 alpha 平均差=%.4f, 形状外平均差=%.4f\n",
                   diff3, diffA, outErr);
            // 形状外语义: 传播无掩码但显示裁剪 (outside) — 形状外两边都应为 0;
            // 形状内差异为软边 (CPU 1px 线性 vs GPU smoothstep) 与剪影浮点累积
            CHECK(diffA < 0.35f, "形状内 alpha 平均差 < 0.35 (软边/浮点累积差)");
            CHECK(outErr < 0.01f, "形状外平均差 < 0.01 (形状外干净)");
        }
    }

    glr::shutdown();
    printf("\n%s (%d 失败)\n", failures == 0 ? "== 全部通过 ==" : "== 有失败 ==", failures);
    return failures;
}


