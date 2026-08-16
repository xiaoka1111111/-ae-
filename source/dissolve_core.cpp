/*
 * dissolve_core.cpp — 填充算法核心实现
 * 数学公式实现自 GLSL 330 着色器语义 (见 gl_shaders.h 与 docs/01-填充算法原理.md)
 */
#include "dissolve_core.h"
#include <algorithm>
#include <cstring>

namespace dissolve {

// ================= Stage 1: Simplex 3D =================
// 与实现 GLSL 逐行一致 (Gustavson 实现)
static inline float mod289f(float x) { return x - std::floor(x * (1.0f / 289.0f)) * 289.0f; }
static inline float permute_f(float x) {
    // mod289(((x*34.0)+10.0)*x)
    float t = (x * 34.0f + 10.0f) * x;
    return t - std::floor(t * (1.0f / 289.0f)) * 289.0f;
}

float simplex3d(float x, float y, float z) {
    // 常量 (GLSL: C = vec2(1/6, 1/3), D = vec4(0, 0.5, 1, 2))
    const float C1 = 1.0f / 6.0f;
    const float C2 = 1.0f / 3.0f;

    // First corner
    float s = (x + y + z) * C2;          // dot(v, C.yyy)
    float i0x = std::floor(x + s);
    float i0y = std::floor(y + s);
    float i0z = std::floor(z + s);

    float x0 = x - i0x + (i0x + i0y + i0z) * C1;   // v - i + dot(i, C.xxx)
    float y0 = y - i0y + (i0x + i0y + i0z) * C1;
    float z0 = z - i0z + (i0x + i0y + i0z) * C1;

    // Other corners (step(x0.yzx, x0.xyz))
    float gx = y0 >= x0 ? 1.f : 0.f;   // step(y0, x0)? GLSL step(edge,x): edge<=x -> 1
    float gy = z0 >= y0 ? 1.f : 0.f;
    float gz = x0 >= z0 ? 1.f : 0.f;
    // GLSL: g = step(x0.yzx, x0.xyz) -> g.x = step(y0,x0), g.y = step(z0,y0), g.z = step(x0,z0)
    gx = y0 <= x0 ? 1.f : 0.f;
    gy = z0 <= y0 ? 1.f : 0.f;
    gz = x0 <= z0 ? 1.f : 0.f;
    float lx = 1.0f - gx, ly = 1.0f - gy, lz = 1.0f - gz;

    // i1 = min(g.xyz, l.zxy); i2 = max(g.xyz, l.zxy)
    float i1x = std::min(gx, lz), i1y = std::min(gy, lx), i1z = std::min(gz, ly);
    float i2x = std::max(gx, lz), i2y = std::max(gy, lx), i2z = std::max(gz, ly);

    float x1 = x0 - i1x + C1, y1 = y0 - i1y + C1, z1 = z0 - i1z + C1;
    float x2 = x0 - i2x + C2, y2 = y0 - i2y + C2, z2 = z0 - i2z + C2;
    float x3 = x0 - 0.5f,     y3 = y0 - 0.5f,     z3 = z0 - 0.5f;

    // Permutations: i = mod289(i)
    float ii0 = mod289f(i0x), ii1 = mod289f(i0y), ii2 = mod289f(i0z);

    // p = permute(permute(permute(i.z + vec4(0,i1.z,i2.z,1))
    //                        + i.y + vec4(0,i1.y,i2.y,1))
    //                        + i.x + vec4(0,i1.x,i2.x,1))
    float p0 = permute_f(ii2 + 0.0f),        p1 = permute_f(ii2 + i1z),
          p2 = permute_f(ii2 + i2z),         p3 = permute_f(ii2 + 1.0f);
    p0 = permute_f(p0 + ii1 + 0.0f);  p1 = permute_f(p1 + ii1 + i1y);
    p2 = permute_f(p2 + ii1 + i2y);   p3 = permute_f(p3 + ii1 + 1.0f);
    p0 = permute_f(p0 + ii0 + 0.0f);  p1 = permute_f(p1 + ii0 + i1x);
    p2 = permute_f(p2 + ii0 + i2x);   p3 = permute_f(p3 + ii0 + 1.0f);

    // Gradients: 7x7 points over a square, mapped onto an octahedron
    const float n_ = 1.0f / 7.0f;   // 0.142857142857
    // ns = n_ * D.wyz - D.xzx;  D=(0,0.5,1,2), D.wyz=(2,0.5,1), D.xzx=(0,1,0)
    const float nsx = n_ * 2.0f;              // 2/7
    const float nsy = n_ * 0.5f - 1.0f;       // 1/14 - 1 = -13/14  (映射到八面体)
    const float nsz = n_ * 1.0f;              // 1/7
    // j = p - 49*floor(p * ns.z * ns.z)
    float j0 = p0 - 49.0f * std::floor(p0 * nsz * nsz);
    float j1 = p1 - 49.0f * std::floor(p1 * nsz * nsz);
    float j2 = p2 - 49.0f * std::floor(p2 * nsz * nsz);
    float j3 = p3 - 49.0f * std::floor(p3 * nsz * nsz);

    float x_0 = std::floor(j0 * nsz), y_0 = std::floor(j0 - 7.0f * x_0);
    float x_1 = std::floor(j1 * nsz), y_1 = std::floor(j1 - 7.0f * x_1);
    float x_2 = std::floor(j2 * nsz), y_2 = std::floor(j2 - 7.0f * x_2);
    float x_3 = std::floor(j3 * nsz), y_3 = std::floor(j3 - 7.0f * x_3);

    float xx0 = x_0 * nsx + nsy, yy0 = y_0 * nsx + nsy;
    float xx1 = x_1 * nsx + nsy, yy1 = y_1 * nsx + nsy;
    float xx2 = x_2 * nsx + nsy, yy2 = y_2 * nsx + nsy;
    float xx3 = x_3 * nsx + nsy, yy3 = y_3 * nsx + nsy;

    // h = 1 - abs(x) - abs(y)
    float h0 = 1.0f - std::fabs(xx0) - std::fabs(yy0);
    float h1 = 1.0f - std::fabs(xx1) - std::fabs(yy1);
    float h2 = 1.0f - std::fabs(xx2) - std::fabs(yy2);
    float h3 = 1.0f - std::fabs(xx3) - std::fabs(yy3);

    // b0 = vec4(x.xy, y.xy); b1 = vec4(x.zw, y.zw)
    float b0x = xx0, b0y = xx1, b0z = yy0, b0w = yy1;
    float b1x = xx2, b1y = xx3, b1z = yy2, b1w = yy3;

    // s0 = floor(b0)*2+1; s1 = floor(b1)*2+1
    float s00 = std::floor(b0x) * 2.0f + 1.0f, s01 = std::floor(b0y) * 2.0f + 1.0f;
    float s02 = std::floor(b0z) * 2.0f + 1.0f, s03 = std::floor(b0w) * 2.0f + 1.0f;
    float s10 = std::floor(b1x) * 2.0f + 1.0f, s11 = std::floor(b1y) * 2.0f + 1.0f;
    float s12 = std::floor(b1z) * 2.0f + 1.0f, s13 = std::floor(b1w) * 2.0f + 1.0f;

    // sh = -step(h, 0)
    float sh0 = h0 <= 0.f ? -1.f : 0.f;
    float sh1 = h1 <= 0.f ? -1.f : 0.f;
    float sh2 = h2 <= 0.f ? -1.f : 0.f;
    float sh3 = h3 <= 0.f ? -1.f : 0.f;

    // a0 = b0.xzyw + s0.xzyw * sh.xxyy
    float a0x = b0x + s00 * sh0, a0y = b0z + s02 * sh0;
    float a0z = b0y + s01 * sh1, a0w = b0w + s03 * sh1;
    // a1 = b1.xzyw + s1.xzyw * sh.zzww
    float a1x = b1x + s10 * sh2, a1y = b1z + s12 * sh2;
    float a1z = b1y + s11 * sh3, a1w = b1w + s13 * sh3;

    // p0..p3 = vec3(a.xy, h)
    float p0x = a0x, p0y = a0y, p0z = h0;
    float p1x = a0z, p1y = a0w, p1z = h1;
    float p2x = a1x, p2y = a1y, p2z = h2;
    float p3x = a1z, p3y = a1w, p3z = h3;

    // Normalise gradients (taylorInvSqrt)
    float d0 = p0x*p0x + p0y*p0y + p0z*p0z;
    float d1 = p1x*p1x + p1y*p1y + p1z*p1z;
    float d2 = p2x*p2x + p2y*p2y + p2z*p2z;
    float d3 = p3x*p3x + p3y*p3y + p3z*p3z;
    float n0 = 1.79284291400159f - 0.85373472095314f * d0;
    float n1 = 1.79284291400159f - 0.85373472095314f * d1;
    float n2 = 1.79284291400159f - 0.85373472095314f * d2;
    float n3 = 1.79284291400159f - 0.85373472095314f * d3;
    p0x *= n0; p0y *= n0; p0z *= n0;
    p1x *= n1; p1y *= n1; p1z *= n1;
    p2x *= n2; p2y *= n2; p2z *= n2;
    p3x *= n3; p3y *= n3; p3z *= n3;

    // m = max(0.5 - dot(x,x), 0); m = m*m; return 105 * dot(m*m, dot(p,x))
    float m0 = std::max(0.5f - (x0*x0 + y0*y0 + z0*z0), 0.f);
    float m1 = std::max(0.5f - (x1*x1 + y1*y1 + z1*z1), 0.f);
    float m2 = std::max(0.5f - (x2*x2 + y2*y2 + z2*z2), 0.f);
    float m3 = std::max(0.5f - (x3*x3 + y3*y3 + z3*z3), 0.f);
    m0 *= m0; m1 *= m1; m2 *= m2; m3 *= m3;

    float n0v = p0x*x0 + p0y*y0 + p0z*z0;
    float n1v = p1x*x1 + p1y*y1 + p1z*z1;
    float n2v = p2x*x2 + p2y*y2 + p2z*z2;
    float n3v = p3x*x3 + p3y*y3 + p3z*z3;

    return 105.0f * (m0*m0*n0v + m1*m1*n1v + m2*m2*n2v + m3*m3*n3v);
}

// ============ Stage 1b: FBM 旋转八度 (rot1/rot2/rot3) ============
float fbm3d(const Params& p, float x, float y, float z) {
    // 与 GLSL 常量逐位一致
    const float rot1[9] = {-0.37f, 0.36f, 0.85f, -0.14f, -0.93f, 0.34f, 0.92f, 0.01f, 0.4f};
    const float rot2[9] = {-0.55f, -0.39f, 0.74f, 0.33f, -0.91f, -0.24f, 0.77f, 0.12f, 0.63f};
    const float rot3[9] = {-0.71f, 0.52f, -0.47f, -0.08f, -0.72f, -0.68f, -0.7f, -0.45f, 0.56f};

    auto apply = [&](const float* m, float mx, float my, float mz) {
        return simplex3d(m[0]*mx + m[1]*my + m[2]*mz,
                         m[3]*mx + m[4]*my + m[5]*mz,
                         m[6]*mx + m[7]*my + m[8]*mz);
    };

    float result = 0.5333333f * apply(rot1, x, y, z);
    if (p.complexityL >= 2)
        result += 0.2666667f * apply(rot2, 2.0f*x, 2.0f*y, 2.0f*z);
    if (p.complexityL >= 3)
        result += 0.1333333f * apply(rot3, 4.0f*x, 4.0f*y, 4.0f*z);
    if (p.complexityL >= 4)
        result += 0.0666667f * simplex3d(8.0f*x, 8.0f*y, 8.0f*z);
    return result;
}

// ============ Stage 1c: 噪声图 (UV 变换链 + 亮度/对比度) ============
void generateNoiseMap(const Params& p, Buffers& buf, int w, int h) {
    buf.resize(w, h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            // GLSL: uv = texCoord; uv -= 0.5; uv += layerOffset;
            //       uv.x *= scaleX; uv.y *= scaleY; uv /= noiseScale;
            //       uv += 0.5; uv -= userOffset; uv.x *= aspect;
            float u = (float)x / (float)w;
            float v = (float)y / (float)h;
            u -= 0.5f; v -= 0.5f;
            u += p.layerOffsetX; v += p.layerOffsetY;
            u *= p.scaleX; v *= p.scaleY;
            u /= p.noiseScale; v /= p.noiseScale;
            u += 0.5f; v += 0.5f;
            u -= p.userOffsetX; v -= p.userOffsetY;
            u *= p.aspect;

            // p3 = vec3(uv, evolution*0.0015)
            float t = p.evolution * 0.0015f;
            float value = fbm3d(p, u * 8.0f + 8.0f, v * 8.0f + 8.0f, t * 8.0f + 8.0f);

            // 亮度/对比度 (ghettoContrast 曲线, 与 GLSL 一致)
            float brightnessMod = (p.brightness - 0.5f) * 0.5f;
            brightnessMod *= p.contrast;
            value = brightnessMod + p.contrast * value;
            value -= (1.0f - p.brightness) * 1.0f;
            value += p.brightness * 1.5f;

            buf.noiseMap[(size_t)y * w + x] = std::min(std::max(value, 0.0f), 1.0f);
        }
    }
}

// ============ Stage 2: Sobel 边缘 ============
void sobelEdges(const float* alpha, Buffers& buf, int w, int h) {
    static const float sobel1[9] = { 1, 0, -1,  2, 0, -2,  1, 0, -1 };
    static const float sobel2[9] = { 1, 2,  1,  0, 0,  0, -1,-2, -1 };
    buf.edgeMap.assign((size_t)w * h, 0.f);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float gx = 0, gy = 0;
            for (int iy = -1; iy <= 1; iy++) {
                for (int ix = -1; ix <= 1; ix++) {
                    int cx = std::min(std::max(x + ix, 0), w - 1);
                    int cy = std::min(std::max(y + iy, 0), h - 1);
                    float val = alpha[(size_t)cy * w + cx];
                    int k = (iy + 1) * 3 + (ix + 1);
                    gx += val * sobel1[k];
                    gy += val * sobel2[k];
                }
            }
            buf.edgeMap[(size_t)y * w + x] = std::sqrt(gx * gx + gy * gy);
        }
    }
}

// ============ Stage 3: JFA 距离场 ============
static inline float unitBoxSdf(float px, float py, float cx, float cy) {
    // GLSL: a = abs(p-c); return length(max(a,0)) + min(max(a.x,a.y), 0)
    float ax = std::fabs(px - cx), ay = std::fabs(py - cy);
    float l = std::sqrt(std::max(ax, 0.f) * std::max(ax, 0.f) +
                        std::max(ay, 0.f) * std::max(ay, 0.f));
    return l + std::min(std::max(ax, ay), 0.f);
}

void jfaDistance(const Params& p, const float* alpha, Buffers& buf, int w, int h) {
    size_t n = (size_t)w * h;
    buf.jfaSeeds.assign(n, 0xFFFFFFFFu);
    buf.distField.assign(n, 0.f);

    // k=0 初始化: 与 GLSL 一致 — 外部像素 = 种子 (x+1)<<16 | (y+1), 1px padding;
    // 内部像素 =  (等待传播); 内部边界像素 = 图像外种子
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float a = alpha[(size_t)y * w + x];
            bool outside;
            if (p.dfModeL == 1) outside = (a <= 1.0f - p.alphaThreshold);
            else                outside = (std::fabs(a) < p.alphaThreshold);
            size_t i = (size_t)y * w + x;
            if (outside) {
                // 外部 = 种子
                buf.jfaSeeds[i] = ((uint32_t)(x+1) << 16) | (uint32_t)(y+1);
            } else {
                // 内部: 默认等待传播; 图像边界像素 -> 图像外种子 (nearest border -1/+1)
                buf.jfaSeeds[i] = 0xFFFFFFFFu;
                if (x == 0)      buf.jfaSeeds[i] = ((uint32_t)(x)     << 16) | (uint32_t)(y+1);
                else if (y == 0) buf.jfaSeeds[i] = ((uint32_t)(x+1)   << 16) | (uint32_t)(y);
                else if (x == w-1) buf.jfaSeeds[i] = ((uint32_t)(x+2) << 16) | (uint32_t)(y+1);
                else if (y == h-1) buf.jfaSeeds[i] = ((uint32_t)(x+1) << 16) | (uint32_t)(y+2);
            }
        }
    }

    // 迭代: 步长 k = max(w,h)/2 减半 (经典 JFA; 升级: 1+JFA 见报告)
    std::vector<uint32_t> cur = buf.jfaSeeds, nxt = buf.jfaSeeds;
    int step = std::max(w, h) / 2;
    while (step >= 1) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                float best = 1e30f;
                uint32_t bestSeed = 0xFFFFFFFFu;
                for (int j = -1; j <= 1; j++) {
                    for (int i = -1; i <= 1; i++) {
                        int sx = x + i * step, sy = y + j * step;
                        if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
                        uint32_t s = cur[(size_t)sy * w + sx];
                        if (s == 0xFFFFFFFFu) continue;
                        float cx = (float)((s >> 16) & 0xFFFF) - 1.0f;
                        float cy = (float)(s & 0xFFFF) - 1.0f;
                        float d = unitBoxSdf((float)x, (float)y, cx, cy);
                        if (d < best) { best = d; bestSeed = s; }
                    }
                }
                nxt[(size_t)y * w + x] = bestSeed;
            }
        }
        cur.swap(nxt);
        step >>= 1;
    }

    // 解码: 距离场 (像素)
    for (size_t i = 0; i < n; i++) {
        uint32_t s = cur[i];
        if (s == 0xFFFFFFFFu) { buf.distField[i] = 0.f; continue; }
        float cx = (float)((s >> 16) & 0xFFFF) - 1.0f;
        float cy = (float)(s & 0xFFFF) - 1.0f;
        float px = (float)(i % w), py = (float)(i / w);
        buf.distField[i] = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
    }
    // 归一化到 0-1 (dist/255 风格, 但按对角线归一化更适合)
    float diag = std::sqrt((float)(w*w + h*h));
    for (size_t i = 0; i < n; i++) buf.distField[i] = std::min(buf.distField[i] / diag, 1.0f);
}

// ============ Stage 4: 覆盖率 + 双时间戳 ============
// 双线性权重 (GLSL sampleweights)
static inline void sampleWeights(float inx, float iny, float* w) {
    float px = std::floor(inx), py = std::floor(iny);
    float fx = inx - px, fy = iny - py;
    float fx1 = 1.0f - fx, fy1 = 1.0f - fy;
    w[0] = fx1 * fy1; w[1] = fx * fy1; w[2] = fx1 * fy; w[3] = fx * fy;
}

// 覆盖率解析积分 (GLSL 8x8=64 次采样的数学等价优化, 见报告 3.2)
float coverageAnalytic(float f00, float f10, float f01, float f11,
                       int numSamples, float eps) {
    if (numSamples <= 1) {
        // 单点采样 (中心)
        float w[4];
        sampleWeights(0.5f, 0.5f, w);
        float v = f00*w[0] + f10*w[1] + f01*w[2] + f11*w[3];
        return v > eps ? 1.0f : 0.0f;
    }
    // 双线性插值 v(px,py) = sum w_i(px,py) f_i, 其中 w_i 是分片线性函数
    // 覆盖率 = (1/N^2) * sum over grid of [v > eps]
    // 精确做法: 对每个角 f_i, 求其在 [0,1]^2 上满足 v>=eps 的区域面积 (解析)
    // 简化精确版: 直接网格累加 (与 GLSL 一致, 但可并行/向量化)
    int N = numSamples;
    int cnt = 0;
    for (int ny = 0; ny < N; ny++) {
        for (int nx = 0; nx < N; nx++) {
            float px = (float)nx / (float)N;
            float py = (float)ny / (float)N;
            float w[4];
            sampleWeights(px, py, w);
            float v = f00*w[0] + f10*w[1] + f01*w[2] + f11*w[3];
            if (v > eps) cnt++;
        }
    }
    return (float)cnt / (float)(N * N);
}

void growthStep(const Params& p, const Buffers& buf, int w, int h,
                const float* prevTime1, const float* prevCov1,
                const float* prevTime2, const float* prevCov2,
                float* outTime1, float* outCov1,
                float* outTime2, float* outCov2) {
    const float EPS = 0.002f;
    int ns = std::max(1, p.numSamples);  // 防御: 避免除零
    const float minCov = 1.0f / (float)(ns * ns);
    // 首帧 (prevTime1==nullptr): 使用调用方传入的零缓冲
    // 注意: 不使用 static (线程安全); fullPipeline 负责提供零缓冲
    if (!prevTime1) {
        // 调用方未提供 -> 内部零缓冲 (每次调用新建, 无共享状态)
        std::vector<float> z1((size_t)w*h, 0.f), z2((size_t)w*h, 0.f);
        std::vector<float> z3((size_t)w*h, 0.f), z4((size_t)w*h, 0.f);
        growthStep(p, buf, w, h, z1.data(), z2.data(), z3.data(), z4.data(),
                   outTime1, outCov1, outTime2, outCov2);
        return;
    }
    const float* fill = buf.fillMap.empty() ? buf.noiseMap.data() : buf.fillMap.data();

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            float pt1 = prevTime1[i], pc1 = prevCov1[i];
            float pt2 = prevTime2[i], pc2 = prevCov2[i];

            float t1 = pt1, c1 = pc1, t2 = pt2, c2 = pc2;

            bool recompute = (pt1 < EPS) ||
                (pt1 >= EPS && pc1 < 1.0f && pt2 < EPS);
            if (recompute) {
                // 4 邻域填充值 (双线性)
                int px = x, py = y;
                int x1 = std::min(x+1, w-1), y1 = std::min(y+1, h-1);
                float f00 = fill[(size_t)py*w+px], f10 = fill[(size_t)py*w+x1];
                float f01 = fill[(size_t)y1*w+px], f11 = fill[(size_t)y1*w+x1];
                float cov = coverageAnalytic(f00, f10, f01, f11, ns, EPS);

                if (cov >= minCov) {
                    if (pc1 < minCov) {
                        t1 = 1.0f + p.timeF; c1 = cov;         // 第一次波前
                    } else {
                        t2 = 1.0f + p.timeF; c2 = 1.0f - pc1;  // 第二次波前
                    }
                } else if (p.cullB == 1) {
                    t1 = 0; c1 = 0; t2 = 0; c2 = 0;            // 剔除
                }
            }
            outTime1[i] = t1; outCov1[i] = c1;
            outTime2[i] = t2; outCov2[i] = c2;
        }
    }
}

// ============ Stage 5b: 时间戳重采样 (GLSL sampletime 精确实现) ============
void sampletimeResample(const float* time1, const float* cov1,
                        const float* time2, const float* cov2,
                        float xformX, float xformY,
                        int w, int h,
                        float* outTime1, float* outCov1,
                        float* outTime2, float* outCov2) {
    if (xformX <= 0.f) xformX = 1.f;
    if (xformY <= 0.f) xformY = 1.f;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            // GLSL: fragLoc / xform (从 dst 映射到 src)
            float in_x = (float)x / xformX;
            float in_y = (float)y / xformY;
            int px = (int)std::floor(in_x);
            int py = (int)std::floor(in_y);
            float fx = in_x - px, fy = in_y - py;
            float fx1 = 1.f - fx, fy1 = 1.f - fy;

            // 4 邻居 (clamp 到边界)
            int x0 = std::min(std::max(px, 0), w-1), x1 = std::min(std::max(px+1, 0), w-1);
            int y0 = std::min(std::max(py, 0), h-1), y1 = std::min(std::max(py+1, 0), h-1);
            size_t i00 = (size_t)y0*w+x0, i10 = (size_t)y0*w+x1;
            size_t i01 = (size_t)y1*w+x0, i11 = (size_t)y1*w+x1;

            // ---- time1/cov1 通道 ----
            {
                float w0 = fx1*fy1, w1 = fx*fy1, w2 = fx1*fy, w3 = fx*fy;
                // coverage 先插值 (GLSL: v.y = dot(...))
                float cv = time1[i00]*0 + cov1[i00]*w0 + cov1[i10]*w1 + cov1[i01]*w2 + cov1[i11]*w3;
                // 未覆盖邻居 (time1 < 1.0) 权重清零
                if (time1[i00] < 1.f) w0 = 0.f;
                if (time1[i10] < 1.f) w1 = 0.f;
                if (time1[i01] < 1.f) w2 = 0.f;
                if (time1[i11] < 1.f) w3 = 0.f;
                float tw = w0+w1+w2+w3;
                float tv = 0.f;
                if (tw > 0.f) {
                    w0 /= tw; w1 /= tw; w2 /= tw; w3 /= tw;
                    tv = time1[i00]*w0 + time1[i10]*w1 + time1[i01]*w2 + time1[i11]*w3;
                    tv = std::max(1.f, tv);  // 时间单调
                }
                outTime1[(size_t)y*w+x] = tv;
                outCov1[(size_t)y*w+x] = cv;
            }
            // ---- time2/cov2 通道 (同逻辑, 用 time2 判未覆盖) ----
            {
                float w0 = fx1*fy1, w1 = fx*fy1, w2 = fx1*fy, w3 = fx*fy;
                float cv = time2[i00]*0 + cov2[i00]*w0 + cov2[i10]*w1 + cov2[i01]*w2 + cov2[i11]*w3;
                if (time2[i00] < 1.f) w0 = 0.f;
                if (time2[i10] < 1.f) w1 = 0.f;
                if (time2[i01] < 1.f) w2 = 0.f;
                if (time2[i11] < 1.f) w3 = 0.f;
                float tw = w0+w1+w2+w3;
                float tv = 0.f;
                if (tw > 0.f) {
                    w0 /= tw; w1 /= tw; w2 /= tw; w3 /= tw;
                    tv = time2[i00]*w0 + time2[i10]*w1 + time2[i01]*w2 + time2[i11]*w3;
                    tv = std::max(1.f, tv);
                }
                outTime2[(size_t)y*w+x] = tv;
                outCov2[(size_t)y*w+x] = cv;
            }
        }
    }
}

// ============ Stage 5c: Fill_GPU 合成 (speedOverlay + borderControl + gamma/exposure) ============
static inline float overlayBlend2(float bg, float fg) {
    return bg < 0.5f ? (2.0f * bg * fg) : (1.0f - 2.0f * (1.0f - bg) * (1.0f - fg));
}

static inline float gammaCorrect2(float v, float gammaF, float exposureF) {
    if (v < 1.0e-4f) v = 0.0f;
    v = std::pow(v, gammaF);
    v *= exposureF;
    return std::min(std::max(v, 0.0f), 1.0f);
}

// [旧链序已推翻, R23] 本函数的链序 speedOverlay→borderControl→gamma 是旧错误顺序;
// 权威链序 = 模糊→gamma/exposure→speedOverlay→borderControl (fill 链 , A 级),
// 见 dissolve_direct.cpp renderPresetDirect。本函数仅旧 renderPreset/tests 死代码路径调用。
void fillComposite(const Params& p, const Buffers& buf,
                   const float* fillMapIn, int w, int h, float* outFill) {
    size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; i++) {
        float v0 = fillMapIn[i];
        // speedOverlay: overlay(v0, speedVal), mix 到 v0
        float speedVal = buf.speedMap.empty() ? 0.f : buf.speedMap[i];
        float v1 = overlayBlend2(v0, speedVal);
        v1 = v1 * p.speedMapInfluenceF + v0 * (1.f - p.speedMapInfluenceF);
        v1 = std::min(std::max(v1, 0.f), 1.f);
        // borderControl: max(border, bridge), mix
        float borderVal = buf.edgeMap.empty() ? 0.f : buf.edgeMap[i];
        float bridgeVal = 0.f;  // bridges 纹理未生成 (后续)
        borderVal = std::max(borderVal, bridgeVal);
        float v2 = borderVal * v1 * p.borderInfluenceF + v1 * (1.f - p.borderInfluenceF);
        v2 = std::min(std::max(v2, 0.f), 1.f);
        // gamma/exposure (设计: 独立 gamma shader , 合并于此)
        outFill[i] = gammaCorrect2(v2, p.gammaF, p.exposureF);
    }
}

// 速度图生成 (设计 SpeedMap 内核 / 实现, 从源图像素 ARGB)
// 实证 (-速度图参数.md §1.5-1.7):
//   mode = 调用方 rdx 参数 (GrowthDrawCPU 主路径 = 参数 ID 0xd, 值域 {1,2}; 无第三模式)
//   scale = 位深常量 (8bit=255.0@, float=1.0@), 非 UI 参数
//   主通道固定读 R 位置 (设计面板 Channel=亮度|Alpha 2 项 → 预处理后写入 R 位置:
//   Luma 预处理 → 内核 R·(G+B+A)/(3·scale²), Alpha 同理)
//   channel: 0=亮度(Luma) 1=Alpha — 设计面板仅这 2 项 (-面板参数完整性.md §4.4)
void speedMapFromSource(const float* srcRGBA, int w, int h, int mode, int channel,
                        float scale, std::vector<float>& speedMap) {
    speedMap.assign((size_t)w * h, 0.f);
    if (!srcRGBA || scale <= 0.f) return;
    const float invScale = 1.f / scale;
    const float invScale2 = invScale * invScale;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        float R = srcRGBA[i*4+0], G = srcRGBA[i*4+1];
        float B = srcRGBA[i*4+2], A = srcRGBA[i*4+3];
        // 主通道 (R 位置): 0=Luma (0.299/0.587/0.114 标准), 1=Alpha
        float main = (channel == 1) ? A : (0.299f*R + 0.587f*G + 0.114f*B);
        // 其余通道固定 (G+B+A)/3 (内核 -: B·(G+R+A)/scale²/3.0)
        float restAvg = (G + B + A) / 3.f;
        float val;
        if (mode == 1) {
            // main * avg(G,B,A)/scale^2 ( mode1)
            val = (main * restAvg) * invScale2;
        } else if (mode == 2) {
            // main/scale ( mode2)
            val = main * invScale;
        } else {
            val = 0.f;  // mode ∉ {1,2} → 输出恒 0 (实证, 无第三模式)
        }
        if (val < 0.001f) val = 0.f;  // 阈值 
        val = std::min(std::max(val, 0.f), 1.f);
        speedMap[i] = val;
    }
}

// ============ Stage 5: 合成 ============
static inline float overlayBlend(float bg, float fg) {
    return bg < 0.5f ? (2.0f * bg * fg) : (1.0f - 2.0f * (1.0f - bg) * (1.0f - fg));
}

static inline float gammaCorrect(float v, float gammaF, float exposureF) {
    if (v < 1.0e-4f) v = 0.0f;
    v = std::pow(v, gammaF);
    v *= exposureF;
    return std::min(std::max(v, 0.0f), 1.0f);
}

void composite(const Params& p, const Buffers& buf, const float* srcAlpha,
               int w, int h, float* outFill) {
    size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; i++) {
        // 速度图影响: overlay 混合 (speedMap 由噪声图+距离场组合模拟)
        float speed = buf.noiseMap[i];
        float v0 = buf.distField[i];
        float v1 = overlayBlend(v0, speed);
        v1 = v1 * p.speedMapInfluenceF + v0 * (1.0f - p.speedMapInfluenceF);
        v1 = std::min(std::max(v1, 0.f), 1.f);

        // 边框控制 (简化: 边缘图加权)
        float border = buf.edgeMap[i];
        float v2 = v1 * (1.0f - p.borderInfluenceF) + border * v1 * p.borderInfluenceF;
        v2 = std::min(std::max(v2, 0.f), 1.f);

        // 时间戳驱动: 已填充区域 = 波前时间归一化 (溶解效果主体)
        float fill = (buf.time1[i] > 0.002f) ? 1.0f : 0.0f;
        // 时间演变淡入
        if (buf.time1[i] > 0.002f) {
            fill = std::min(buf.time1[i] / 60.0f, 1.0f);  // 60 帧内完成溶解
        }
        v2 *= fill;

        // alpha 裁剪 + gamma
        float a = srcAlpha[i];
        v2 *= a;
        outFill[i] = gammaCorrect(v2, p.gammaF, p.exposureF);
    }
}

// ============ 全管线 (单帧参考) ============
void fullPipeline(const Params& p, const float* srcRGBA, int w, int h,
                  float* outRGBA, Buffers& scratch) {
    std::vector<float> alpha((size_t)w*h);
    for (size_t i = 0; i < (size_t)w*h; i++) alpha[i] = srcRGBA[i*4+3];

    generateNoiseMap(p, scratch, w, h);
    sobelEdges(alpha.data(), scratch, w, h);
    jfaDistance(p, alpha.data(), scratch, w, h);

    // 生长: 首帧
    std::vector<float> t1((size_t)w*h), c1((size_t)w*h), t2((size_t)w*h), c2((size_t)w*h);
    scratch.time1.swap(t1); scratch.cov1.swap(c1);
    scratch.time2.swap(t2); scratch.cov2.swap(c2);
    growthStep(p, scratch, w, h, nullptr, nullptr, nullptr, nullptr,
               scratch.time1.data(), scratch.cov1.data(),
               scratch.time2.data(), scratch.cov2.data());

    composite(p, scratch, alpha.data(), w, h, scratch.fillMap.data());

    for (size_t i = 0; i < (size_t)w*h; i++) {
        float f = scratch.fillMap[i];
        outRGBA[i*4+0] = srcRGBA[i*4+0] * (1.0f - f) + f * 0.0f;  // 溶解为透明
        outRGBA[i*4+1] = srcRGBA[i*4+1] * (1.0f - f) + f * 0.0f;
        outRGBA[i*4+2] = srcRGBA[i*4+2] * (1.0f - f) + f * 0.0f;
        outRGBA[i*4+3] = srcRGBA[i*4+3] * (1.0f - f);
    }
}

} // namespace dissolve
