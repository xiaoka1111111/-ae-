/* gpu_bench.cpp — GPU 路径基准: 全图 vs 区域, 分阶段计时 */
#include "../gl_renderer.h"
#include "../dissolve_core.h"
#include "../dissolve_styles.h"
#include "../preset_data.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <chrono>
using namespace dissolve;
using clk = std::chrono::steady_clock;

int main() {
    printf("== GPU 基准 ==\n");
    if (!glr::init()) { printf("GL init FAIL\n"); return 1; }
    float noiseParams[7] = {1.f, 1.f, 1.f, 0.5f, 1.f, 0.f, 1.f};
    const Preset& pres = kPresets[0];
    struct Case { int w, h; const char* name; };
    Case cases[] = { {4000, 4000, "全图 4000x4000"}, {3744, 563, "区域 3744x563"}, {64, 64, "预览 64x64"} };
    for (auto& cs : cases) {
        std::vector<float> src((size_t)cs.w*cs.h*4, 0.f);
        for (size_t i = 0; i < src.size(); i += 4) { src[i+3] = 1.f; src[i] = 0.2f; src[i+1]=0.4f; src[i+2]=0.6f; }
        // 预热
        std::vector<float> out;
        glr::renderFrame(src.data(), cs.w, cs.h, pres, 50.f, noiseParams, 4,
                         0.5f, 0.5f, 1.f, 1.f, 1.f, 0, out);
        double t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0;
        const int N = 5;
        for (int k = 0; k < N; k++) {
            auto a = clk::now();
            bool ok = glr::renderFrame(src.data(), cs.w, cs.h, pres, 50.f, noiseParams, 4,
                                       0.5f, 0.5f, 1.f, 1.f, 1.f, 0, out);
            auto b = clk::now();
            if (!ok) { printf("%s: renderFrame FAIL\n", cs.name); break; }
            double ms = std::chrono::duration<double, std::milli>(b - a).count();
            if (k == 0) t0 = ms; else { t1 += ms; t2++; }
        }
        printf("%-18s 首帧 %.1f ms, 平均 %.1f ms\n", cs.name, t0, t1 / std::max(t2, 1.0));
        fflush(stdout);
    }
    printf("DONE\n");
    return 0;
}
