// gl_mini.cpp — 最小 GL 环境诊断
#include <windows.h>
#include <GL/gl.h>
#include <cstdio>
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

int main() {
    printf("== GL 最小诊断 ==\n");
    WNDCLASSA wc = {};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "gl_mini";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "gl_mini", "x", WS_OVERLAPPEDWINDOW, 0,0,8,8,0,0,wc.hInstance,0);
    printf("[1] window: %s\n", hwnd ? "OK" : "FAIL");
    HDC hdc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32;
    int pf = ChoosePixelFormat(hdc, &pfd);
    printf("[2] pixel format: %d (%s)\n", pf, pf ? "OK" : "FAIL");
    if (!SetPixelFormat(hdc, pf, &pfd)) { printf("[2] SetPixelFormat FAIL\n"); return 1; }
    HGLRC rc = wglCreateContext(hdc);
    printf("[3] wglCreateContext: %s\n", rc ? "OK" : "FAIL");
    if (!wglMakeCurrent(hdc, rc)) { printf("[3] MakeCurrent FAIL\n"); return 1; }
    printf("[4] GL version: %s\n", (const char*)glGetString(GL_VERSION));
    printf("[5] GL renderer: %s\n", (const char*)glGetString(GL_RENDERER));
    // GL 1.1 基础测试
    glClearColor(0.3f, 0.5f, 0.7f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char px[4] = {0};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("[6] clear+readback: (%u, %u, %u) %s\n", px[0], px[1], px[2],
           (px[0] > 60 && px[0] < 90) ? "OK" : "UNEXPECTED");
    printf("== done ==\n");
    return 0;
}
