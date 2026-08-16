/*
 * gl_renderer.cpp — GPU 渲染器实现 (OpenGL 3.3 Core, 隐藏窗口)
 * GL 函数: 基础 1.1 从 opengl32.dll 导入, 其余 wglGetProcAddress
 */
#include "gl_renderer.h"
#include "gl_shaders.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <mutex>
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// ---------- GL 常量 (最小集) ----------
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_R32F 0x822E
#define GL_RGBA32F 0x8814
#define GL_RED 0x1903
#define GL_RGBA 0x1908
#define GL_FLOAT 0x1406
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_TEXTURE2 0x84C2
#define GL_TEXTURE3 0x84C3
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_TRIANGLES 0x0004
#define GL_UNSIGNED_BYTE 0x1401
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

// ---------- GL 函数指针 ----------
#define GLPROC(ret, name, params) typedef ret (APIENTRY* PFN_##name) params; static PFN_##name p##name = nullptr;
GLPROC(void, glViewport, (int, int, int, int))
GLPROC(void, glDrawArrays, (unsigned, int, int))
GLPROC(void, glClear, (unsigned))
GLPROC(void, glClearColor, (float, float, float, float))
GLPROC(void, glReadPixels, (int, int, int, int, unsigned, unsigned, void*))
GLPROC(void, glFinish, (void))
GLPROC(void, glGenTextures, (int, unsigned*))
GLPROC(void, glDeleteTextures, (int, const unsigned*))
GLPROC(void, glBindTexture, (unsigned, unsigned))
GLPROC(void, glTexImage2D, (unsigned, int, int, int, int, int, unsigned, unsigned, const void*))
GLPROC(void, glTexParameteri, (unsigned, unsigned, int))
GLPROC(void, glActiveTexture, (unsigned))
GLPROC(void, glGenFramebuffers, (int, unsigned*))
GLPROC(void, glBindFramebuffer, (unsigned, unsigned))
GLPROC(void, glFramebufferTexture2D, (unsigned, unsigned, unsigned, unsigned, int))
GLPROC(unsigned, glCheckFramebufferStatus, (unsigned))
GLPROC(unsigned, glCreateShader, (unsigned))
GLPROC(void, glShaderSource, (unsigned, int, const char* const*, const int*))
GLPROC(void, glCompileShader, (unsigned))
GLPROC(void, glGetShaderiv, (unsigned, unsigned, int*))
GLPROC(void, glGetShaderInfoLog, (unsigned, int, int*, char*))
GLPROC(void, glDeleteShader, (unsigned))
GLPROC(unsigned, glCreateProgram, (void))
GLPROC(void, glAttachShader, (unsigned, unsigned))
GLPROC(void, glBindAttribLocation, (unsigned, unsigned, const char*))
GLPROC(void, glLinkProgram, (unsigned))
GLPROC(void, glGetProgramiv, (unsigned, unsigned, int*))
GLPROC(void, glGetProgramInfoLog, (unsigned, int, int*, char*))
GLPROC(void, glDeleteProgram, (unsigned))
GLPROC(void, glUseProgram, (unsigned))
GLPROC(int, glGetUniformLocation, (unsigned, const char*))
GLPROC(void, glUniform1f, (int, float))
GLPROC(void, glUniform1i, (int, int))
GLPROC(void, glUniform2f, (int, float, float))
GLPROC(void, glUniform1fv, (int, int, const float*))
GLPROC(void, glUniform1iv, (int, int, const int*))
GLPROC(void, glUniform4fv, (int, int, const float*))
GLPROC(void, glGenVertexArrays, (int, unsigned*))
GLPROC(void, glBindVertexArray, (unsigned))
GLPROC(void, glGenBuffers, (int, unsigned*))
GLPROC(void, glBindBuffer, (unsigned, unsigned))
GLPROC(void, glBufferData, (unsigned, int, const void*, unsigned))
GLPROC(void, glVertexAttribPointer, (unsigned, int, unsigned, unsigned char, int, const void*))
GLPROC(void, glEnableVertexAttribArray, (unsigned))
typedef HGLRC (APIENTRY* PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int*);
GLPROC(HGLRC, wglGetCurrentContext, (void))
GLPROC(HDC, wglGetCurrentDC, (void))

namespace glr {

// 全局互斥: 保护 init/shutdown/renderFrame 的所有 GL 状态访问
// (AE 可能多线程调用; 即使当前未声明 THREADED_RENDERING 也保持安全)
static std::mutex g_glMutex;

static HWND g_hwnd = nullptr;
static HDC g_hdc = nullptr;
static HGLRC g_glrc = nullptr;
static bool g_ok = false;
static bool g_initFailed = false;  // 初始化失败后永久禁用 (禁止每帧重试)

static LogFn g_logFn = nullptr;

// AE 内 printf 不可见 -> 同时走回调 (回调缺省时仍 printf, 独立测试可见)
static void glLog(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    if (g_logFn) g_logFn(buf);
}

static unsigned g_progNoise = 0, g_progSobel = 0, g_progJFA = 0, g_progLayers = 0;
static unsigned g_vao = 0, g_vbo = 0;
static unsigned g_texAlpha = 0, g_texNoise = 0, g_texJFA[2] = {0,0}, g_texDist = 0, g_texSrc = 0, g_texOut = 0, g_texEdge = 0;
static unsigned g_fbo = 0;
static int g_texW = 0, g_texH = 0;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcA(h, m, w, l);
}

// 加载 GL 函数: 1.1 从 opengl32.dll, 其余 wglGetProcAddress
static void* getGL(const char* name) {
    void* p = (void*)wglGetProcAddress(name);
    if (p) return p;
    return (void*)GetProcAddress(GetModuleHandleA("opengl32.dll"), name);
}
#define LOAD(name) do { p##name = (PFN_##name)getGL(#name); if (!p##name) { glLog("[GL] missing %s", #name); return false; } } while(0)

static bool loadGL() {
    LOAD(glViewport); LOAD(glDrawArrays); LOAD(glClear); LOAD(glClearColor);
    LOAD(glReadPixels); LOAD(glFinish);
    LOAD(glGenTextures); LOAD(glDeleteTextures); LOAD(glBindTexture);
    LOAD(glTexImage2D); LOAD(glTexParameteri); LOAD(glActiveTexture);
    LOAD(glGenFramebuffers); LOAD(glBindFramebuffer); LOAD(glFramebufferTexture2D);
    LOAD(glCheckFramebufferStatus);
    LOAD(glCreateShader); LOAD(glShaderSource); LOAD(glCompileShader);
    LOAD(glGetShaderiv); LOAD(glGetShaderInfoLog); LOAD(glDeleteShader);
    LOAD(glCreateProgram); LOAD(glAttachShader); LOAD(glBindAttribLocation);
    LOAD(glLinkProgram); LOAD(glGetProgramiv); LOAD(glGetProgramInfoLog);
    LOAD(glDeleteProgram); LOAD(glUseProgram); LOAD(glGetUniformLocation);
    LOAD(glUniform1f); LOAD(glUniform1i); LOAD(glUniform2f);
    LOAD(glUniform1fv); LOAD(glUniform1iv); LOAD(glUniform4fv);
    LOAD(glGenVertexArrays); LOAD(glBindVertexArray); LOAD(glGenBuffers);
    LOAD(glBindBuffer); LOAD(glBufferData); LOAD(glVertexAttribPointer);
    LOAD(glEnableVertexAttribArray);
    LOAD(wglGetCurrentContext); LOAD(wglGetCurrentDC);
    return true;
}

static bool createContext() {
    WNDCLASSA wc = {};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "TestFill_GL_Win_Class";
    RegisterClassA(&wc);
    g_hwnd = CreateWindowExA(0, wc.lpszClassName, "gl", WS_OVERLAPPEDWINDOW,
                             0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) return false;
    g_hdc = GetDC(g_hwnd);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    int pf = ChoosePixelFormat(g_hdc, &pfd);
    if (!pf || !SetPixelFormat(g_hdc, pf, &pfd)) return false;
    HGLRC tmp = wglCreateContext(g_hdc);
    if (!tmp) return false;
    wglMakeCurrent(g_hdc, tmp);
    PFN_wglCreateContextAttribsARB ca = (PFN_wglCreateContextAttribsARB)wglGetProcAddress("wglCreateContextAttribsARB");
    if (!ca) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(tmp); return false; }
    // 尝试 3.3 Core, 失败降级 3.3 兼容 (虚拟机/远程桌面/部分驱动 core profile 不可用)
    const int attribsCore[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0
    };
    const int attribsCompat[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        0
    };
    g_glrc = ca(g_hdc, nullptr, attribsCore);
    if (!g_glrc) g_glrc = ca(g_hdc, nullptr, attribsCompat);
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(tmp);
    if (!g_glrc) return false;
    if (!wglMakeCurrent(g_hdc, g_glrc)) return false;
    return true;
}

static unsigned compileProgram(const char* vs, const char* fs) {
    unsigned v = pglCreateShader(GL_VERTEX_SHADER);
    unsigned f = pglCreateShader(GL_FRAGMENT_SHADER);
    pglShaderSource(v, 1, &vs, nullptr);
    pglShaderSource(f, 1, &fs, nullptr);
    pglCompileShader(v);
    pglCompileShader(f);
    int vok = 0, fok = 0;
    pglGetShaderiv(v, GL_COMPILE_STATUS, &vok);
    pglGetShaderiv(f, GL_COMPILE_STATUS, &fok);
    if (!vok || !fok) {
        char log[2048];
        if (!vok) { pglGetShaderInfoLog(v, sizeof(log), nullptr, log); glLog("[GL] VS err: %s", log); }
        if (!fok) { pglGetShaderInfoLog(f, sizeof(log), nullptr, log); glLog("[GL] FS err: %s", log); }
        pglDeleteShader(v); pglDeleteShader(f);
        return 0;
    }
    unsigned prog = pglCreateProgram();
    pglAttachShader(prog, v);
    pglAttachShader(prog, f);
    pglBindAttribLocation(prog, 0, "vertex");
    pglBindAttribLocation(prog, 1, "tex");
    pglLinkProgram(prog);
    int lok = 0;
    pglGetProgramiv(prog, GL_LINK_STATUS, &lok);
    pglDeleteShader(v); pglDeleteShader(f);
    if (!lok) {
        char log[2048];
        pglGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        glLog("[GL] LINK err: %s", log);
        pglDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static void mkTex(unsigned& t, int w, int h) {
    pglGenTextures(1, &t);
    pglBindTexture(GL_TEXTURE_2D, t);
    pglTexImage2D(GL_TEXTURE_2D, 0, (int)GL_R32F, w, h, 0, GL_RED, GL_FLOAT, nullptr);
    pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// RGBA 源图纹理 (mode=3 原图显现层)
static void mkTexRGBA(unsigned& t, int w, int h) {
    pglGenTextures(1, &t);
    pglBindTexture(GL_TEXTURE_2D, t);
    pglTexImage2D(GL_TEXTURE_2D, 0, (int)GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static bool ensureResources(int w, int h) {
    if (g_texW == w && g_texH == h) return true;
    if (g_texAlpha) {
        pglDeleteTextures(1, &g_texAlpha);
        pglDeleteTextures(1, &g_texNoise);
        pglDeleteTextures(2, g_texJFA);
        pglDeleteTextures(1, &g_texDist);
        pglDeleteTextures(1, &g_texSrc);
        pglDeleteTextures(1, &g_texOut);
        pglDeleteTextures(1, &g_texEdge);
    }
    mkTex(g_texAlpha, w, h);
    mkTex(g_texNoise, w, h);
    mkTex(g_texJFA[0], w, h);
    mkTex(g_texJFA[1], w, h);
    mkTex(g_texDist, w, h);
    mkTexRGBA(g_texSrc, w, h);
    mkTexRGBA(g_texOut, w, h);
    mkTex(g_texEdge, w, h);
    if (!g_fbo) pglGenFramebuffers(1, &g_fbo);
    if (!g_vao) {
        pglGenVertexArrays(1, &g_vao);
        pglGenBuffers(1, &g_vbo);
        pglBindVertexArray(g_vao);
        pglBindBuffer(GL_ARRAY_BUFFER, g_vbo);
        float verts[] = { -1,-1, 0,0,   3,-1, 2,0,   -1,3, 0,2 };
        pglBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
        pglVertexAttribPointer(0, 2, GL_FLOAT, 0, 4*sizeof(float), (void*)0);
        pglEnableVertexAttribArray(0);
        pglVertexAttribPointer(1, 2, GL_FLOAT, 0, 4*sizeof(float), (void*)(2*sizeof(float)));
        pglEnableVertexAttribArray(1);
    }
    g_texW = w; g_texH = h;
    return true;
}

static void runPass(unsigned prog, unsigned outTex) {
    pglBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    pglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outTex, 0);
    pglViewport(0, 0, g_texW, g_texH);
    pglUseProgram(prog);
    pglBindVertexArray(g_vao);
    pglDrawArrays(GL_TRIANGLES, 0, 3);
}

static int U(unsigned prog, const char* name) { return pglGetUniformLocation(prog, name); }

// 内部实现 (调用方必须持有 g_glMutex)
// 注意: 失败必须设置 g_initFailed — 否则 AE 每帧重试 init (建窗口+编译 shader), 巨卡
static bool initLocked() {
    if (g_ok) return true;
    if (g_initFailed) return false;
    if (!createContext()) { glLog("[GL] context fail (wglCreateContextAttribsARB 3.3 core 不可用)"); g_initFailed = true; return false; }
    if (!loadGL()) { glLog("[GL] funcs fail"); g_initFailed = true; return false; }
    g_progNoise = compileProgram(kVSFullscreen, kNoiseFS);
    g_progSobel = compileProgram(kVSFullscreen, kSobelFS);
    g_progJFA = compileProgram(kVSFullscreen, kJFAFS);
    g_progLayers = compileProgram(kVSFullscreen, kLayersFS);
    if (!g_progNoise || !g_progSobel || !g_progJFA || !g_progLayers) {
        // 清理部分创建的 shader, 避免泄漏
        if (g_progNoise) { pglDeleteProgram(g_progNoise); g_progNoise = 0; }
        if (g_progSobel) { pglDeleteProgram(g_progSobel); g_progSobel = 0; }
        if (g_progJFA) { pglDeleteProgram(g_progJFA); g_progJFA = 0; }
        if (g_progLayers) { pglDeleteProgram(g_progLayers); g_progLayers = 0; }
        glLog("[GL] shader compile fail");
        g_initFailed = true;
        return false;
    }
    g_ok = true;
    return true;
}

bool init() {
    std::lock_guard<std::mutex> lk(g_glMutex);
    return initLocked();
}

void setLogFn(LogFn fn) {
    std::lock_guard<std::mutex> lk(g_glMutex);
    g_logFn = fn;
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_glMutex);
    if (g_glrc) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(g_glrc); g_glrc = nullptr; }
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = nullptr; }
    g_ok = false;
}

bool available() {
    std::lock_guard<std::mutex> lk(g_glMutex);
    return g_ok;
}

bool renderFrame(const float* srcRGBA, int w, int h,
                 const Preset& preset, float progress,
                 const float* noiseParams, int complexity,
                 float alphaThreshold,
                 float speedInfluence, float borderInfluence,
                 float gamma, float exposure,
                 int speedMapChannel,
                 std::vector<float>& outRGBA,
                 const float* seedMask,
                 int blendMode) {
    std::lock_guard<std::mutex> lk(g_glMutex);
    if (!g_ok && !initLocked()) return false;
    // 防御: 尺寸/指针检查必须在 ensureResources (创建纹理) 之前
    if (w <= 0 || h <= 0 || !srcRGBA) return false;
    // 多线程: AE 多线程渲染, 只有 init 线程有 current context.
    // 每帧 makeCurrent 本上下文, 结束后恢复调用方线程之前的上下文 (不干扰 AE 自身 GL).
    HGLRC prevCtx = pwglGetCurrentContext();
    HDC prevDC = pwglGetCurrentDC();
    if (!wglMakeCurrent(g_hdc, g_glrc)) {
        glLog("[GL] makeCurrent fail");
        return false;
    }
    if (!ensureResources(w, h)) { wglMakeCurrent(prevDC, prevCtx); return false; }
    // 防御: noiseParams 为空时用默认噪声参数
    float defNoise[7] = {1.f, 1.f, 1.f, 0.5f, 1.f, 0.f, 1.f};
    if (!noiseParams) noiseParams = defNoise;

    // 纹理行序: framebuffer 读回时翻转 Y, 上传时同步翻转行序以保持一致
    // (否则非对称形状的距离场/源图采样会上下颠倒)
    std::vector<float> alpha((size_t)w*h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            alpha[(size_t)y*w+x] = srcRGBA[((size_t)(h-1-y)*w+x)*4+3];
    pglBindTexture(GL_TEXTURE_2D, g_texAlpha);
    pglTexImage2D(GL_TEXTURE_2D, 0, (int)GL_R32F, w, h, 0, GL_RED, GL_FLOAT, alpha.data());
    // 源图 RGBA 纹理 (mode=3 原图显现层) — 同样翻转行序
    std::vector<float> srcFlip((size_t)w*h*4);
    for (int y = 0; y < h; y++)
        memcpy(&srcFlip[(size_t)y*w*4], &srcRGBA[(size_t)(h-1-y)*w*4], (size_t)w*4*sizeof(float));
    pglBindTexture(GL_TEXTURE_2D, g_texSrc);
    pglTexImage2D(GL_TEXTURE_2D, 0, (int)GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, srcFlip.data());

    // Pass 1: 噪声
    pglUseProgram(g_progNoise);
    pglUniform1f(U(g_progNoise, "noiseScale"), noiseParams[0]);
    pglUniform1f(U(g_progNoise, "scaleX"), noiseParams[1]);
    pglUniform1f(U(g_progNoise, "scaleY"), noiseParams[2]);
    pglUniform1f(U(g_progNoise, "brightness"), noiseParams[3]);
    pglUniform1f(U(g_progNoise, "contrast"), noiseParams[4]);
    pglUniform1f(U(g_progNoise, "evolution"), noiseParams[5]);
    pglUniform1f(U(g_progNoise, "aspect"), noiseParams[6]);
    pglUniform1i(U(g_progNoise, "complexityL"), complexity);
    pglUniform1f(U(g_progNoise, "ghettoContrast"), noiseParams[4]);
    runPass(g_progNoise, g_texNoise);

    // Pass 1.5: Sobel 边缘 (borderTexture 来源, Fill_GPU borderControl 使用)
    pglUseProgram(g_progSobel);
    pglActiveTexture(GL_TEXTURE0);
    pglBindTexture(GL_TEXTURE_2D, g_texAlpha);
    pglUniform1i(U(g_progSobel, "tex"), 0);
    runPass(g_progSobel, g_texEdge);

    // Pass 2: JFA 距离场 (k=0 + 迭代, 末轮解码)
    int maxStep = std::max(w, h) / 2;
    pglUseProgram(g_progJFA);
    pglActiveTexture(GL_TEXTURE0);
    pglBindTexture(GL_TEXTURE_2D, g_texAlpha);
    pglUniform1i(U(g_progJFA, "in_tex"), 0);
    pglUniform2f(U(g_progJFA, "jfa_res"), (float)w, (float)h);
    pglUniform1f(U(g_progJFA, "alpha_threshold"), alphaThreshold);
    pglUniform1i(U(g_progJFA, "dfModeL"), 1);
    pglUniform1i(U(g_progJFA, "lastStepB"), 0);
    pglUniform1i(U(g_progJFA, "k"), 0);
    runPass(g_progJFA, g_texJFA[0]);
    int cur = 0, nxt = 1;
    while (maxStep >= 1) {
        pglActiveTexture(GL_TEXTURE1);
        pglBindTexture(GL_TEXTURE_2D, g_texJFA[cur]);
        pglUniform1i(U(g_progJFA, "jfa_tex"), 1);
        pglUniform1i(U(g_progJFA, "k"), maxStep);
        pglUniform1i(U(g_progJFA, "lastStepB"), (maxStep == 1) ? 1 : 0);
        runPass(g_progJFA, g_texJFA[nxt]);
        cur ^= 1; nxt ^= 1;
        maxStep >>= 1;
    }
    // 末轮结果在 g_texJFA[cur] (若 maxStep 初始为 1, cur=0) — 边缘距离场读回,
    // 用于自动选点 (形状内部最深点), 再算"到种子点的传播距离场"上传 g_texDist
    float maxB = 1.f;  // BFS 最大距离 (softEdge 用, 块外可见)
    {
        pglBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
        pglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_texJFA[cur], 0);
        pglViewport(0, 0, w, h);
        pglReadPixels(0, 0, w, h, GL_RED, GL_FLOAT, alpha.data());  // 复用缓冲
        // 自动选点: 边缘距离场最大处 (内部最深 = 最大内切圆圆心), 或圆点笔刷多源
        float bestD = -1.f; int sx = w/2, sy = h/2;
        std::vector<float> bfs((size_t)w*h, 1e9f);
        std::vector<int> qx, qy;
        float alphaTh = std::min(std::max(alphaThreshold, 0.01f), 0.5f);
        if (seedMask) {
            // 圆点笔刷多源: 掩码>0.05 且形状内的点全部为种子
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    size_t i = (size_t)y*w+x;
                    if (seedMask[i] > 0.05f && alpha[i] > alphaTh) {
                        bfs[i] = 0.f; qx.push_back(x); qy.push_back(y);
                    }
                }
        } else {
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    float d = alpha[(size_t)y*w+x];
                    if (d > bestD) { bestD = d; sx = x; sy = y; }
                }
            bool seedInside = alpha[(size_t)sy*w+sx] > alphaTh;
            if (seedInside) {
                bfs[(size_t)sy*w+sx] = 0.f; qx.push_back(sx); qy.push_back(sy);
            }
        }
        const int dx[8] = {1,-1,0,0,1,1,-1,-1}, dy[8] = {0,0,1,-1,1,-1,1,-1};
        for (size_t qi = 0; qi < qx.size(); qi++) {
            int x = qx[qi], y = qy[qi];
            float d = bfs[(size_t)y*w+x];
            if (d > maxB) maxB = d;
            for (int k = 0; k < 8; k++) {
                int nx = x+dx[k], ny = y+dy[k];
                if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                size_t ni = (size_t)ny*w+nx;
                if (bfs[ni] > d + 1.f) {
                    if (alpha[ni] <= alphaTh) continue;  // 形状外不传播
                    bfs[ni] = d + 1.f; qx.push_back(nx); qy.push_back(ny);
                }
            }
        }
        // 形状外保持 2.0 (shader: nd clamp 到 1, 但 outside=step(1.001, raw) 强制 fill=0)
        // 否则归一化后形状外 nd=1, 进度 100% 时会被填充 (层色溢出形状边界)
        for (size_t i = 0; i < (size_t)w*h; i++)
            bfs[i] = (bfs[i] >= 1e8f) ? 2.0f : std::min(bfs[i] / maxB, 1.f);
        pglBindTexture(GL_TEXTURE_2D, g_texDist);
        pglTexImage2D(GL_TEXTURE_2D, 0, (int)GL_R32F, w, h, 0, GL_RED, GL_FLOAT, bfs.data());
    }

    // Pass 3: 图层合成
    pglUseProgram(g_progLayers);
    pglActiveTexture(GL_TEXTURE0);
    pglBindTexture(GL_TEXTURE_2D, g_texDist);
    pglUniform1i(U(g_progLayers, "distTex"), 0);
    pglActiveTexture(GL_TEXTURE1);
    pglBindTexture(GL_TEXTURE_2D, g_texNoise);
    pglUniform1i(U(g_progLayers, "noiseTex"), 1);
    pglActiveTexture(GL_TEXTURE2);
    pglBindTexture(GL_TEXTURE_2D, g_texSrc);
    pglUniform1i(U(g_progLayers, "srcTex"), 2);
    pglActiveTexture(GL_TEXTURE3);
    pglBindTexture(GL_TEXTURE_2D, g_texEdge);
    pglUniform1i(U(g_progLayers, "edgeTex"), 3);
    // Fill_GPU 参数 (设计: speedOverlay + borderControl + gamma/exposure)
    pglUniform1f(U(g_progLayers, "speedMapInfluenceF"), speedInfluence);
    pglUniform1f(U(g_progLayers, "borderInfluenceF"), borderInfluence);
    pglUniform1f(U(g_progLayers, "gammaF"), gamma);
    pglUniform1f(U(g_progLayers, "exposureF"), exposure);
    pglUniform1f(U(g_progLayers, "progress"), progress);
    pglUniform1f(U(g_progLayers, "softEdge"), 1.f / std::max(maxB, 1.f));
    pglUniform1i(U(g_progLayers, "speedMapChannel"), speedMapChannel);
    pglUniform1i(U(g_progLayers, "dbgLayer"), 0);  // 0=关闭调试
    pglUniform1i(U(g_progLayers, "nLayers"), preset.nLayers);
    float starts[5] = {}, ends[5] = {}, grows[5] = {}, ops[5] = {};
    int modes[5] = {}, overlays[5] = {}, nStops[5] = {}, gm[5] = {};
    float colors[20] = {}, stops[240] = {}, stopsA[240] = {};
    float displ[5] = {}, dsize[5] = {}, blurs[5] = {};
    // 按预设数据顺序渲染 (设计  层循环无 order 排序; 后画覆盖先画)
    // 层数据按槽位顺序 (0..nLayers-1) 上传, shader 按 li 槽位读取
    int idx[5];
    for (int i = 0; i < 5; i++) idx[i] = i;
    for (int slot = 0; slot < 5; slot++) {
        int i = idx[slot];  // 排序后第 slot 个层 = 原始 layers[i]
        starts[slot] = 0; ends[slot] = 99; modes[slot] = 0; overlays[slot] = 0;
        ops[slot] = 0; nStops[slot] = 0; grows[slot] = 0; gm[slot] = 0;
        colors[slot*4+3] = 1.f;
        if (i < preset.nLayers) {
            const PresetLayer& L = preset.layers[i];
            starts[slot] = L.start; ends[slot] = L.end; grows[slot] = L.grow;
            ops[slot] = L.overlayOpacity; modes[slot] = L.mode;
            // 面板"混合模式"覆盖 (与 CPU renderPreset 同步;  权威映射, 1-based)
            int bm = (L.overlayMode != 0) ? L.overlayMode : (blendMode > 0 ? blendMode : 1);
            overlays[slot] = bm;
            nStops[slot] = L.nStops; gm[slot] = L.gradientMode;
            blurs[slot] = L.blur;
            colors[slot*4+0] = L.color[0]; colors[slot*4+1] = L.color[1];
            colors[slot*4+2] = L.color[2]; colors[slot*4+3] = L.color[3];
            // 拷贝并按 pos 排序 (设计预设数据 pos 乱序)
            PresetColorStop sorted[12];
            int nS = std::min(L.nStops, 12);
            for (int s = 0; s < nS; s++) sorted[s] = L.stops[s];
            for (int a = 0; a < nS; a++)
                for (int b = a+1; b < nS; b++)
                    if (sorted[b].pos < sorted[a].pos) std::swap(sorted[a], sorted[b]);
            for (int s = 0; s < 12; s++) {
                int base = slot*12 + s;
                if (s < nS) {
                    stops[base*4+0] = sorted[s].r; stops[base*4+1] = sorted[s].g;
                    stops[base*4+2] = sorted[s].b; stops[base*4+3] = sorted[s].pos;
                    stopsA[base] = sorted[s].a;  // 颜色 alpha (独立数组, 不占用 pos)
                } else { stops[base*4+3] = 1.f; stopsA[base] = 1.f; }
            }
            displ[slot] = L.displace; dsize[slot] = L.displaceSize;
        }
    }
    pglUniform1fv(U(g_progLayers, "L_start"), 5, starts);
    pglUniform1fv(U(g_progLayers, "L_end"), 5, ends);
    pglUniform1iv(U(g_progLayers, "L_mode"), 5, modes);
    pglUniform4fv(U(g_progLayers, "L_color"), 5, colors);
    pglUniform1iv(U(g_progLayers, "L_nStops"), 5, nStops);
    pglUniform4fv(U(g_progLayers, "L_stops"), 60, stops);
    pglUniform1fv(U(g_progLayers, "L_stopsA"), 60, stopsA);
    pglUniform1fv(U(g_progLayers, "L_grow"), 5, grows);
    pglUniform1iv(U(g_progLayers, "L_overlay"), 5, overlays);
    pglUniform1iv(U(g_progLayers, "L_gradientMode"), 5, gm);
    pglUniform1fv(U(g_progLayers, "L_opacity"), 5, ops);
    pglUniform1fv(U(g_progLayers, "L_blur"), 5, blurs);
    pglUniform1fv(U(g_progLayers, "L_displace"), 5, displ);
    pglUniform1fv(U(g_progLayers, "L_displaceSize"), 5, dsize);
    pglUniform2f(U(g_progLayers, "texelSize"), 1.f/(float)w, 1.f/(float)h);
    runPass(g_progLayers, g_texOut);

    // 回读
    pglBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    pglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_texOut, 0);
    outRGBA.resize((size_t)w*h*4);
    pglReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, outRGBA.data());
    std::vector<float> tmp = outRGBA;
    for (int y = 0; y < h; y++)
        memcpy(&outRGBA[(size_t)y*w*4], &tmp[(size_t)(h-1-y)*w*4], (size_t)w*4*sizeof(float));
    // 恢复调用方线程之前的 GL 上下文 (AE 可能自用 OpenGL)
    wglMakeCurrent(prevDC, prevCtx);
    return true;
}

} // namespace glr
