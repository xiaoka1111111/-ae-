/*
 * dissolve_core.h — 填充算法核心 (纯 C++, 无 AE 依赖)
 *
 * 算法阶段:
 *   Stage 1: Simplex 3D FBM 噪声 (旋转八度抑制各向异性)
 *   Stage 2: Sobel 边缘检测
 *   Stage 3: 跳洪 (JFA) 距离场
 *   Stage 4: 覆盖率采样 + 双时间戳波前生长 (溶解本质)
 *   Stage 5: 速度图/边框合成 + gamma/exposure
 *
 * 数学公式详见 docs/01-填充算法原理.md
 */
#pragma once
#include <cstdint>
#include <vector>
#include <cmath>

namespace dissolve {

// ---------------- 基础类型 ----------------
struct RGBAf { float r, g, b, a; };

struct Params {
    // 噪声场
    float  noiseScale   = 1.0f;   // 用户噪声缩放
    float  scaleX       = 1.0f;   // 图层缩放 X
    float  scaleY       = 1.0f;   // 图层缩放 Y
    float  brightness   = 0.5f;   // 亮度 (0-1)
    float  contrast     = 1.0f;   // "ghettoContrast" 对比度
    float  evolution    = 0.0f;   // 时间演变
    float  aspect       = 1.0f;   // 像素宽高比
    float  layerOffsetX = 0.0f;
    float  layerOffsetY = 0.0f;
    float  userOffsetX  = 0.0f;
    float  userOffsetY  = 0.0f;
    int    complexityL  = 4;      // FBM 八度数 1-4

    // 边缘/距离场
    float  alphaThreshold = 0.5f; // JFA 内外判定阈值
    int    dfModeL        = 1;    // 1: alpha<=1-th 为外, 2: alpha<th 为外

    // 生长填充
    int    numSamples   = 8;      // 覆盖率采样数 (8 = 64 次/像素, 1=1 次)
    float  timeF        = 0.0f;   // 帧时间增量
    int    cullB        = 0;      // 覆盖率不足时剔除

    // 合成
    float  speedMapInfluenceF = 0.5f;
    float  borderInfluenceF   = 0.0f;
    float  gammaF             = 1.0f;
    float  exposureF          = 1.0f;
    int    speedMapMode       = 2;   // 速度图内核模式 (0x332C0: 1=通道×其余平均, 2=单通道; 0=关闭)
    int    speedMapChannel    = 0;   // 速度图通道 (原版面板: 0=亮度 Luma, 1=Alpha — 实证仅 2 项)
    float  speedMapScale      = 1.0f; // 速度缩放 [C] (原版: 位深常量 8bit=255.0/float=1.0,  0x13C774/0x13C694)
    float  blurRadius         = 0.0f; // 全局模糊半径 (面板 Fill > Blur Radius, 作用于 fillMap)
    int    borderExpand       = 0;    // 边框扩展 (面板 Borders > Border Expand, edgeMap 膨胀像素)
    int    blendMode          = 0;    // 面板"混合模式" (原版 12 项选项表 0x1FE350; 0=跟随预设)
    int    growthSource       = 0;    // 面板"生长来源" (原版 点|噪波|图层; 0=点 1=噪波 2=图层)
    int    quality            = 0;    // param 6 质量 popup (完整|一半|双重) → 除数表 {1,1,2,0.5}
    int    sourceMode         = 0;    // 文字模式 (0=原版: mode-3 源图盖顶; 1=填充覆盖文字: 跳过 mode-3 层)
    float  borderStrength     = 1.f;  // 边界强度 0-1 (1=强边界: 传播不越界; 低值允许溢出)
    int    bridgeMode         = 0;    // 桥接模式 (0=无; 非 0 时自动膨胀桥: 连接相邻分离元素)
    int    bridgeThick        = 5;    // 桥接厚度 (像素, 传播掩码额外膨胀量)
    // 噪波生长参数 [2026-08-18]: 面板 6 控件直接映射到上方噪声场字段
    //   (AF_NOISE_SCALE→noiseScale, AF_NOISE_EVOLUTION→evolution,
    //    AF_NOISE_OFFSET_X/Y→userOffsetX/Y, AF_NOISE_CONTRAST→contrast,
    //    AF_NOISE_COMPLEXITY→complexityL) — CPU/GPU 共用同一噪声管线。
};

// ---------------- 中间缓冲区 ----------------
struct Buffers {
    int w = 0, h = 0;
    unsigned long long shapeHash = 0;  // 形状指纹 (getStaticFields 计算; BFS 缓存键用)
    std::vector<float> noiseMap;      // [w*h] 归一化噪声 (0-1)
    std::vector<float> edgeMap;       // [w*h] sobel 边缘
    std::vector<uint32_t> jfaSeeds;   // [w*h] JFA 种子 (x<<16|y 编码)
    std::vector<float> distField;     // [w*h] 距离场 (像素; 仅旧路径/测试用)
    std::vector<float> speedMap;      // [w*h] 速度图
    std::vector<float> time1, cov1;   // [w*h] 第一次波前 时间/覆盖率
    std::vector<float> time2, cov2;   // [w*h] 第二次波前
    std::vector<float> fillMap;       // [w*h] 最终填充

    void resize(int w_, int h_) {
        w = w_; h = h_;
        size_t n = (size_t)w * h;
        noiseMap.assign(n, 0.f);
        edgeMap.assign(n, 0.f);
        jfaSeeds.assign(n, 0xFFFFFFFFu);
        distField.assign(n, 0.f);
        speedMap.assign(n, 0.f);
        time1.assign(n, 0.f); cov1.assign(n, 0.f);
        time2.assign(n, 0.f); cov2.assign(n, 0.f);
        fillMap.assign(n, 0.f);
    }
};

// ---------------- 核心 API ----------------
// Stage 1: Simplex 3D 噪声 (Gustavson, 与还原 GLSL 逐行一致)
float simplex3d(float x, float y, float z);

// Stage 1b: FBM 旋转八度 (rot1/rot2/rot3 与还原 GLSL 一致)
float fbm3d(const Params& p, float x, float y, float z);

// Stage 1c: 噪声图生成 (含 UV 变换链)
void generateNoiseMap(const Params& p, Buffers& buf, int w, int h);

// Stage 2: Sobel 边缘检测 (输入 alpha)
void sobelEdges(const float* alpha, Buffers& buf, int w, int h);

// Stage 3: JFA 距离场 (CPU 版: 编码 -> 迭代 -> 解码)
void jfaDistance(const Params& p, const float* alpha, Buffers& buf, int w, int h);

// Stage 4: 覆盖率 + 双时间戳生长 (单帧推进)
//   prevTime1/prevCov1/prevTime2/prevCov2 为上一帧状态 (首帧传 nullptr)
void growthStep(const Params& p, const Buffers& buf, int w, int h,
                const float* prevTime1, const float* prevCov1,
                const float* prevTime2, const float* prevCov2,
                float* outTime1, float* outCov1,
                float* outTime2, float* outCov2);

// Stage 5: 合成 (速度图影响 + 边框 + gamma/exposure)
void composite(const Params& p, const Buffers& buf, const float* srcAlpha,
               int w, int h, float* outFill);

// Stage 5b: 时间戳重采样 (GLSL sampletime 精确还原)
//   对 time1/cov1/time2/cov2 四通道做双线性插值;
//   关键: 未覆盖邻居 (time<1.0) 权重清零后重归一化 (防时间泄漏穿过未填充区);
//   time 分量 max(1.0, t) 保证单调
//   xform = 速度缩放 (速度图驱动传播)
void sampletimeResample(const float* time1, const float* cov1,
                        const float* time2, const float* cov2,
                        float xformX, float xformY,
                        int w, int h,
                        float* outTime1, float* outCov1,
                        float* outTime2, float* outCov2);

// Stage 5c: Fill_GPU 合成 (speedOverlay + borderControl + gamma/exposure)
//   v0 = videoTexture (fillMap 灰度)
//   v1 = overlay(v0, speedVal) 混合 speedMapInfluenceF
//   v2 = borderControl(v1): max(border, bridge) 混合 borderInfluenceF
//   输出 clamp 0-1
void fillComposite(const Params& p, const Buffers& buf,
                   const float* fillMapIn, int w, int h, float* outFill);

// 速度图生成 (原版 SpeedMap 逐像素内核 0x332C0/0x335C0 ):
//   从源图像素 ARGB 计算速度值 (非距离场!); 面板参数 Channel (截图实证 "Luma")
//   mode1: val = B*avg(R,G,A)/scale^2  (0x33423-0x33491: (G+R+A)/scale, *B/scale, /3.0)
//   mode2: val = B/scale               (0x3349D-0x334B8)
//   channel: 0=Luma(亮度) 1=R 2=G 3=B 4=A — Luma 用 avg(R,G,B), 单通道用对应值
//   val < 0.001 -> 0 (阈值 0x13C638); clamp [0,1]
//   scale: 速度缩放参数 (来源待确认 [C])
void speedMapFromSource(const float* srcRGBA, int w, int h, int mode, int channel,
                        float scale, std::vector<float>& speedMap);

// 便捷: 全管线单帧 (用于测试/参考实现)
void fullPipeline(const Params& p, const float* srcRGBA, int w, int h,
                  float* outRGBA, Buffers& scratch);

// 覆盖率解析积分优化版 (GLSL 64 采样 -> 4 角面积积分, 见报告 3.2)
float coverageAnalytic(float f00, float f10, float f01, float f11,
                       int numSamples, float eps);

} // namespace dissolve
