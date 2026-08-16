# 关于 AE 插件填充算法研究

一个 After Effects 溶解/填充效果插件的完整算法研究、工程实现与原理说明。

## 这是什么

本项目是一个 **AE 效果插件**（After Effects SDK 25.6，C++17）的完整工程：

- **效果名称**：测试（matchName: `ADBE TestFill2`）
- **算法**：以"波前扩散"为核心的填充/溶解效果 —— 从种子点出发，填充波前随时间扫过图层，
  逐层显现颜色、渐变与源图内容，可叠加速度图调制、边框强调、模糊、gamma/曝光等后期链。
- **形态**：GPU 加速渲染（OpenGL 3.3，默认自动，失败回退 CPU）+ CPU 直通渲染管线。

## 仓库结构

```
.
├── README.md                 # 本文件
├── docs/
│   ├── 01-填充算法原理.md      # 算法完整原理（数学、数据流、设计决策）
│   ├── 02-代码结构解答.md      # 代码结构、参数表、核心函数、性能优化
│   └── 03-AE渲染架构.md       # AE 插件渲染架构（渲染路径、区域渲染、缓存、标志位）
└── source/                   # 完整插件源码（可直接构建）
    ├── FillingEffect.cpp      # 插件外壳：参数注册、渲染入口、合成
    ├── dissolve_core.*        # 算法核心：Simplex 噪声、Sobel、距离场、覆盖率生长
    ├── dissolve_styles.*      # 预设多层波次渲染系统（30 预设）
    ├── dissolve_direct.*      # 直通渲染管线：splat/BFS/填充链/层着色
    ├── preset_data.h          # 预设数据（30 个预设，每预设最多 5 层）
    ├── preset_names_cn.h      # 中文预设名
    ├── gl_shaders.h           # GPU 管线 GLSL 源码
    ├── gl_renderer.*          # OpenGL 渲染器
    ├── FillingEffectPiPL.r    # PiPL 资源（插件身份/标志）
    ├── build_manual.bat       # 一键构建脚本（MSVC）
    ├── install_to_AE.bat      # 安装到 AE 插件目录
    └── tests/                 # 回归测试（9 个套件）
```

## 核心算法一句话

> 从种子点生成距离场，波前半径 = 时间进度，填充强度 = 距离场阈值化 + 软边，
> 多层按时间窗口带通着色后 src-over 合成，末端可选速度图/边框/gamma 调制。

## 阅读路线

1. [01-填充算法原理.md](docs/01-填充算法原理.md) —— 先读这篇，理解算法本体
2. [02-代码结构解答.md](docs/02-代码结构解答.md) —— 代码怎么组织、每个函数干什么
3. [03-AE渲染架构.md](docs/03-AE渲染架构.md) —— 插件如何接入 AE 渲染管线

## 构建

1. 安装 After Effects SDK 25.6 与 Visual Studio 2022 BuildTools (v143, C++17)
2. 修改 `source/build_manual.bat` 中的 SDK 路径
3. 运行 `build_manual.bat`，产物为 `build/FillingEffect.aex`
4. 运行 `install_to_AE.bat`（管理员）安装到 AE 插件目录，重启 AE
5. 在 AE 中：效果 → Test → 测试

## 许可

本项目仅供学习研究。MIT。
