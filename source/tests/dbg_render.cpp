/* dbg_render.cpp — 输出渲染结果到 PPM 可视化 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../dissolve_core.h"
#include "../dissolve_styles.h"
#include "../dissolve_direct.h"
#include "../preset_data.h"
#include <cstdio>
#include <vector>

using namespace dissolve;

static void savePPM(const char* path, const float* rgb, int w, int h, const char* tag) {
    FILE* f = fopen(path, "w");
    if (!f) { printf("cannot write %s\n", path); return; }
    fprintf(f, "P3\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t i = ((size_t)y*w+x)*3;
            int r = (int)(rgb[i+0]*255), g = (int)(rgb[i+1]*255), b = (int)(rgb[i+2]*255);
            if (r<0)r=0; if(r>255)r=255; if(g<0)g=0; if(g>255)g=255; if(b<0)b=0; if(b>255)b=255;
            fprintf(f, "%d %d %d ", r, g, b);
        }
        fprintf(f, "\n");
    }
    fclose(f);
    printf("[%s] saved %s\n", tag, path);
}

int main(int argc, char** argv) {
    int presetIdx = argc > 1 ? atoi(argv[1]) : 7;   // Fire
    float prog = argc > 2 ? (float)atof(argv[2]) : 50.f;
    const int W = 160, H = 160;
    // 渐变彩色源图 (非对称, 检查方向)
    std::vector<float> src((size_t)W*H*4, 0.f);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            size_t i = ((size_t)y*W+x)*4;
            src[i+0] = (float)x/W; src[i+1] = (float)y/H; src[i+2] = 1.f-(float)x/W; src[i+3] = 1.f;
        }
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
    float maxD = 0.f;
    for (size_t i = 0; i < (size_t)W*H; i++) maxD = std::max(maxD, buf.distField[i]);
    printf("maxDist=%.4f (对角线归一化)\n", maxD);

    // 1. 距离场可视化
    {
        std::vector<float> img((size_t)W*H*3);
        for (size_t i = 0; i < (size_t)W*H; i++) {
            float v = buf.distField[i] / maxD;
            img[i*3+0]=v; img[i*3+1]=v; img[i*3+2]=v;
        }
        savePPM("out_dist.ppm", img.data(), W, H, "dist");
    }
    // 2. 噪声图可视化
    {
        std::vector<float> img((size_t)W*H*3);
        for (size_t i = 0; i < (size_t)W*H; i++) {
            float v = buf.noiseMap[i];
            img[i*3+0]=v; img[i*3+1]=v; img[i*3+2]=v;
        }
        savePPM("out_noise.ppm", img.data(), W, H, "noise");
    }
    // 3. 预设渲染 (层色 + alpha)
    {
        StyleFrame fr;
        fr.preset = &kPresets[presetIdx];
        fr.progress = prog;
        std::vector<float> cR, cG, cB, cA;
        renderPreset(fr, buf, src.data(), W, H, cR, cG, cB, cA);
        char nm[128];
        snprintf(nm, sizeof(nm), "out_%d_layer.ppm", presetIdx);
        std::vector<float> img((size_t)W*H*3);
        for (size_t i = 0; i < (size_t)W*H; i++) {
            img[i*3+0]=cR[i]; img[i*3+1]=cG[i]; img[i*3+2]=cB[i];
        }
        savePPM(nm, img.data(), W, H, "layer");
        // 4. 最终合成 (compOver)
        snprintf(nm, sizeof(nm), "out_%d_final.ppm", presetIdx);
        std::vector<float> img2((size_t)W*H*3);
        bool compOver = (kPresets[presetIdx].compOverOriginal == 0);
        for (size_t i = 0; i < (size_t)W*H; i++) {
            float a = cA[i];
            float oR, oG, oB;
            if (compOver) {
                oR = src[i*4+0]*(1-a) + cR[i]*a;
                oG = src[i*4+1]*(1-a) + cG[i]*a;
                oB = src[i*4+2]*(1-a) + cB[i]*a;
            } else {
                oR = cR[i]*a + src[i*4+0]*(1-a);
                oG = cG[i]*a + src[i*4+1]*(1-a);
                oB = cB[i]*a + src[i*4+2]*(1-a);
            }
            img2[i*3+0]=oR; img2[i*3+1]=oG; img2[i*3+2]=oB;
        }
        savePPM(nm, img2.data(), W, H, "final");
        printf("preset=%d(%s) prog=%.0f compOver=%d\n",
               presetIdx, kPresets[presetIdx].name, prog, (int)compOver);
    }
    // 5. 新参数路径冒烟: 噪波生长种子 + blendMode 覆盖 + speedMapMode 无
    {
        // 5a. 噪波种子 (growthSource=1): 需手动生成噪波种子掩码并验证渲染不崩
        {
            std::vector<float> seedMask((size_t)W*H, 0.f);
            for (size_t i = 0; i < (size_t)W*H; i++)
                if (buf.noiseMap[i] > 0.5f) seedMask[i] = 1.f;
            StyleFrame fr;
            fr.preset = &kPresets[7];   // Fire
            fr.progress = 50.f;
            fr.seedMask = seedMask.data();
            std::vector<float> cR, cG, cB, cA;
            renderPreset(fr, buf, src.data(), W, H, cR, cG, cB, cA);
            size_t lit = 0;
            for (size_t i = 0; i < (size_t)W*H; i++) if (cA[i] > 0.05f) lit++;
            printf("[5a] 噪波种子渲染: 内容像素=%zu/%zu\n", lit, (size_t)W*H);
            if (lit == 0) { printf("  !! 噪波种子无内容\n"); return 1; }
        }
        // 5b. blendMode 覆盖 (1=正常, 0x3D830 权威映射) + speedMapMode=0
        {
            Params p2 = p;
            p2.blendMode = 1;
            p2.speedMapMode = 0;
            p2.speedMapInfluenceF = 0.f;
            StyleFrame fr;
            fr.preset = &kPresets[7];
            fr.progress = 50.f;
            fr.params = &p2;
            std::vector<float> cR, cG, cB, cA;
            renderPreset(fr, buf, src.data(), W, H, cR, cG, cB, cA);
            float maxA = 0.f;
            for (size_t i = 0; i < (size_t)W*H; i++) maxA = std::max(maxA, cA[i]);
            printf("[5b] blendMode=1+speedMapMode=0 渲染: maxAlpha=%.3f\n", maxA);
            if (maxA <= 0.f) { printf("  !! 无内容\n"); return 1; }
        }
        printf("[5] 新参数路径冒烟 OK\n");
    }
    // 6. 直通管线渲染冒烟 (GrowthDrawCPU 0x18060): 验证直通版有合理内容输出
    {
        DirectFrame dfr;
        dfr.preset = &kPresets[presetIdx];
        dfr.progress01 = prog / 100.f;
        dfr.totalFrames = kPresets[presetIdx].duration * 30.f;  // 假设 30fps
        dfr.splatRadius = 10.f;
        dfr.rampS = 1.f;
        dfr.srcRGBA = src.data();
        dfr.noiseFill = buf.noiseMap.data();
        int nL = std::min(kPresets[presetIdx].nLayers, 5);
        std::vector<float> pts((size_t)nL*2, 0.f), th(nL, 0.f);
        for (int li = 0; li < nL; li++) {
            pts[(size_t)li*2+0] = (float)W * 0.5f;
            pts[(size_t)li*2+1] = (float)H * 0.5f;
            th[li] = kPresets[presetIdx].layers[li].start / 100.f;
        }
        dfr.layerPts = pts.data();
        dfr.layerThresh = th.data();
        std::vector<float> dR, dG, dB, dA;
        renderPresetDirect(dfr, W, H, dR, dG, dB, dA);
        size_t lit = 0;
        float rSum = 0.f, gSum = 0.f, bSum = 0.f;
        for (size_t i = 0; i < (size_t)W*H; i++)
            if (dA[i] > 0.05f) { lit++; rSum += dR[i]; gSum += dG[i]; bSum += dB[i]; }
        printf("[6] 直通管线: 内容像素=%zu/%zu 平均色=(%.3f,%.3f,%.3f)\n",
               lit, (size_t)W*H,
               lit ? rSum/(float)lit : 0.f,
               lit ? gSum/(float)lit : 0.f,
               lit ? bSum/(float)lit : 0.f);
        if (lit == 0) { printf("  !! 直通管线无内容\n"); return 1; }
        // 输出 PPM 可视化
        std::vector<float> img((size_t)W*H*3);
        for (size_t i = 0; i < (size_t)W*H; i++) {
            img[i*3+0]=dR[i]; img[i*3+1]=dG[i]; img[i*3+2]=dB[i];
        }
        savePPM("out_direct.ppm", img.data(), W, H, "direct");
    }
    return 0;
}
