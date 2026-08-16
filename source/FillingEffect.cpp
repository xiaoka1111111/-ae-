/*
 * FillingEffect.cpp — AE 溶解填充效果插件 (After Effects SDK 25.6)
 *
 * 算法总览:
 *   Simplex FBM 噪声 -> Sobel 边缘 -> 距离场 -> 波前扩散生长 -> 多层着色合成
 *
 * 完整原理见仓库 docs/01-填充算法原理.md 与 docs/02-代码结构解答.md。
 */
#include "AEConfig.h"

#ifdef AE_OS_WIN
	#define NOMINMAX
	#include <Windows.h>
#endif

#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectCBSuites.h"
#include "AE_EffectSuites.h"
#include "AE_EffectPixelFormat.h"
#include "PrSDKAESupport.h"
#include "AEFX_SuiteHelper.h"
#include "Smart_Utils.h"
#include "Param_Utils.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <vector>
#include <algorithm>
#include "dissolve_core.h"
#include "dissolve_styles.h"
#include "dissolve_direct.h"
#include "preset_data.h"
#include "preset_names_cn.h"
#include "gl_renderer.h"

#define NAME				"测试"
#define DESCRIPTION			"溶解/填充效果实现 (算法实现自 填充效果)"
#define MAJOR_VERSION		2
#define MINOR_VERSION		1
#define BUG_VERSION			0
#define STAGE_VERSION		PF_Stage_DEVELOP
// 参数布局已多次变更 (质量滑块→下拉/延迟类型/新增4参数/混合模式11→17项),
// my_version 不变时 AE 缓存旧参数注册与 ECW 面板 → 旧工程 UI 下拉无法提交 (预设切换无效)。
// 升 BUILD_VERSION 强制 AE 重建参数注册。
// v4 (2026-08-16): matchName 改为 TestFill2 — 效果身份彻底变化, 旧磁盘缓存全部失效
//   (实测: AE 缓存键不含参数布局/版本, 只含图层内容+时间 — "不清缓存没反应")
#define BUILD_VERSION		4

#define NUM_PARAMS			AF_NUM_PARAMS_TOTAL

// ---------- 调试日志 (写入 %TEMP%\TestFill_debug.log) ----------
#include <mutex>
static std::mutex g_logMutex;
static long g_logCount = 0;
static void logMsg(const char* fmt, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	std::lock_guard<std::mutex> lk(g_logMutex);  // 多线程渲染时保护文件写入
	char path[MAX_PATH];
	if (GetTempPathA(MAX_PATH, path)) {
		strncat(path, "TestFill_debug.log", MAX_PATH - strlen(path) - 1);
		// 日志上限 ~8MB: 超限截断重写 (长时间播放/多会话累积会无限增长)
		WIN32_FILE_ATTRIBUTE_DATA fi;
		if (GetFileAttributesExA(path, GetFileExInfoStandard, &fi) &&
		    fi.nFileSizeHigh == 0 && fi.nFileSizeLow > 8u * 1024 * 1024)
			DeleteFileA(path);
		FILE* f = fopen(path, "a");
		if (f) { fprintf(f, "[%lu] %s\n", GetCurrentThreadId(), buf); fclose(f); }
	}
}
// GL 渲染器日志回调 (glr 内部无可变参, 包装转发)
static void glLogSink(const char* msg) { logMsg("%s", msg); }

// 降频日志: 只记录开头 + 每 60 帧一条 + 错误/关键事件 (大尺寸逐帧写盘会拖慢渲染)
static void logFrame(const char* fmt, ...) {	long n = InterlockedIncrement(&g_logCount);
	if (n <= 8 || (n % 60) == 0 || n < 0) {
		char buf[1024];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
		logMsg("%s", buf);
	}
}

// 参数枚举 — 对齐设计面板顺序 (截图 OCR 实证; PF_ADD_TOPIC 参数组在 AE 渲染
// 兼容性待验证, 暂用扁平顺序 — 后续验证通过再恢复折叠)
enum {
	AF_INPUT = 0,
	AF_PRESET,           // 预设
	AF_QUALITY,          // 质量 (param 6: 完整|一半|双重 → 除数表)
	AF_POINT_COUNT,      // 生长点数 1-5
	AF_RADIUS,           // 笔刷半径
	AF_POS1, AF_POS2, AF_POS3, AF_POS4, AF_POS5,  // 点位置 x5
	AF_DELAY,            // 点延迟 (帧, 设计"延迟（帧）" 0-100)
	AF_SPEED,            // 生长速度 (per sec, 默认 100)
	AF_SMAP_MODE,        // 速度图模式 (设计 6 项: 无|较慢近边界|自定义图层|基于形状的流动|湍流噪波|都)
	AF_SMAP_INFLUENCE,   // 速度图影响
	AF_CHANNEL,          // 通道 (设计仅 亮度|Alpha 2 项 — 实证 )
	AF_BORDER_STRENGTH,  // 边框强度
	AF_BORDER_EXPAND,    // 边框扩展
	AF_BRIDGE_MODE,      // 桥接模式 (设计 无|遮罩|图层 — 实证 )
	AF_BRIDGE_THICK,     // 桥接厚度
	AF_BLUR_RADIUS,      // 模糊半径
	AF_EXPOSURE,         // 曝光
	AF_DURATION,         // 时长 (0=用预设)
	AF_REPEAT,           // 重复 (0=用预设)
	AF_COMPOSITE,        // 叠于原图 (0=自动 1=叠 2=替换)
	AF_RENDERER,         // 渲染器 CPU|GPU|Auto
	AF_VIEW,             // 调试视图: 关|生长|速度图|边框
	AF_ABOUT,
	// ---- 追加参数 (放在尾部: 保持上述索引不变, 旧 AE 工程参数值不错位) ----
	AF_GROWTH_SOURCE,    // 生长来源 (设计 点|噪波|图层 — 实证 )
	AF_BLEND_MODE,       // 混合模式 (设计 12 项选项表 ; 0=跟随预设)
	AF_DELAY2, AF_DELAY3, AF_DELAY4, AF_DELAY5,  // 点 2..5 延迟 (设计 param 32/34/36/38)
	AF_LOOP_FADE,        // 循环淡出 0-100% (周期末尾平滑淡出强度; 0=硬回绕原语义)
	AF_SOURCE_MODE,      // 文字模式: 0=设计(源图盖顶, 文字保持原色) 1=填充覆盖文字
	AF_NUM_PARAMS_TOTAL,
	// ---- 参数组 ID (PF_ADD_TOPIC 不占 params[] 索引, 用 100+ 段) ----
	AF_GROUP_GROWTH = 100,
	AF_GROUP_SPEEDMAP,
	AF_GROUP_BORDERS,
	AF_GROUP_FILL,
	AF_GROUP_PIPELINE,
	AF_GROUP_END_GROWTH = 110,
	AF_GROUP_END_SPEEDMAP,
	AF_GROUP_END_BORDERS,
	AF_GROUP_END_FILL,
	AF_GROUP_END_PIPELINE
};

static PF_Err
About(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*)
{
	PF_SPRINTF(out_data->return_msg, "%s, v%d.%d\r%s",
		NAME, MAJOR_VERSION, MINOR_VERSION, DESCRIPTION);
	return PF_Err_NONE;
}

static PF_Err
GlobalSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*)
{
	out_data->my_version = PF_VERSION(MAJOR_VERSION, MINOR_VERSION, BUG_VERSION,
		STAGE_VERSION, BUILD_VERSION);

	out_data->out_flags = PF_OutFlag_PIX_INDEPENDENT |
		PF_OutFlag_DEEP_COLOR_AWARE |
		PF_OutFlag_NON_PARAM_VARY;
	// PF_OutFlag_NON_PARAM_VARY [修正 2026-08-16 — 语义曾理解反了!]:
	//   SDK 原文: "Set this if the effect uses information other than the
	//   parameters in the param list to generate its output at the current time.
	//   For instance, if the effect uses the current time of the frame..."
	//   → 设置 = 声明"输出依赖时间等参数之外的信息" → AE 每帧都重渲, 不跨时间缓存。
	//   → 不设置 = AE 认为"参数不变则输出不变" → 跨时间只渲一帧并复用!
	//   实测症状 (用户连续 3 天反馈): 播放/拖时间线无动画 (参数无关键帧 → AE 复用
	//   首帧), 只有拖动图层 (输入变化 → 强制重渲) 才刷新。RQ 渲染逐帧强制不受影响,
	//   所以探针一直"正常" — 交互路径才是 AE 的真实行为。
	//   旧注释"绝对不能设置"是错误理解, 已删。PiPL Global_OutFlags 同步加 0x4。

	// 传统 Render 路径 (不回 SmartRender):
	//   SmartRender 区域渲染时 AE 可能请求变换后区域 (output≠src 尺寸, 实测 23x7 vs 50x55),
	//   实现的左上对齐导致内容偏移到坐标 0,0。设计是老式插件 (AEGP Iterate 依据),
	//   传统 Render 下 AE 保证 src==output==图层像素尺寸, 效果正确铺满图层。
	//   因此不设置 PF_OutFlag2_SUPPORTS_SMART_RENDER (连带不设 FLOAT_COLOR_AWARE,
	//   AE 2026 严格校验两者绑定)。
	// 参数组折叠: 回退 — PF_ADD_TOPIC 组参数实际占 params[] 索引 (PF_ADD_PARAM(-1) 追加),
	// 会导致 num_params 计数不匹配 (AE 25::34) 且效果失效; 出点标记 2 同步复杂。
	// 面板折叠对齐留待后续正确实现 (组参数计入 num_params)。
	out_data->out_flags2 = 0;

	if (in_data->appl_id == kAppID_Premiere) {
		AEFX_SuiteScoper<PF_PixelFormatSuite1> pfs =
			AEFX_SuiteScoper<PF_PixelFormatSuite1>(in_data, kPFPixelFormatSuite,
				kPFPixelFormatSuiteVersion1, out_data);
		(*pfs->ClearSupportedPixelFormats)(in_data->effect_ref);
		(*pfs->AddSupportedPixelFormat)(in_data->effect_ref, PrPixelFormat_VUYA_4444_32f);
		(*pfs->AddSupportedPixelFormat)(in_data->effect_ref, PrPixelFormat_BGRA_4444_32f);
		(*pfs->AddSupportedPixelFormat)(in_data->effect_ref, PrPixelFormat_VUYA_4444_8u);
		(*pfs->AddSupportedPixelFormat)(in_data->effect_ref, PrPixelFormat_BGRA_4444_8u);
	}
	return PF_Err_NONE;
}

static PF_Err
ParamsSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef*)
{
	PF_Err err = PF_Err_NONE;
	PF_ParamDef def;

	// ---- Pipeline: 预设 (设计列表 = None/Reset + 30 预设 32 项, 运行时实测) ----
	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUPX("预设", kPresetPopupCount, 2, kPresetPopupItemsCN, 0, AF_PRESET);

	// ---- Quality (设计 param 6: 完整|一半|双重 popup, A 级 实现-参数来源.md §1.1;
	//      除数表 {1.0,1.0,2.0,0.5}@, 3 选项用 0..2) ----
	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUPX("质量", 3, 0, "完整|一半|双重", 0, AF_QUALITY);

	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUPX("生长点数", 5, 0, "1 点|2 点|3 点|4 点|5 点", 0, AF_POINT_COUNT);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("笔刷半径", 1.0f, 500.0f, 1.0f, 500.0f, 10.0f, 1, 0, 0, AF_RADIUS);

	AEFX_CLR_STRUCT(def);
	// 设计默认 {45,45} = 45% 图层宽高 ( 16.16 定点, A 级 实现-参数来源 §1.1)
	// 修正 [2026-08-16]: PF_ADD_POINT(45,45) 的 FLOAT2FIX 默认值会被 AE 按"百分比→
	// 像素"换算 (新工程实测默认读出 144%/81% — 320×180 图层的 45%×320=144px),
	// 导致新工程点位置错位 (heal 只修越界 x, y=81% 偏下)。
	// 用 PRECONVERTED 直接传 16.16 定点, 保持 45% 语义。
	PF_ADD_POINT_PRECONVERTED("点 1 位置", FLOAT2FIX(45), FLOAT2FIX(45), 0, AF_POS1);

	AEFX_CLR_STRUCT(def);
	PF_ADD_POINT_PRECONVERTED("点 2 位置", FLOAT2FIX(45), FLOAT2FIX(45), 0, AF_POS2);

	AEFX_CLR_STRUCT(def);
	PF_ADD_POINT_PRECONVERTED("点 3 位置", FLOAT2FIX(45), FLOAT2FIX(45), 0, AF_POS3);

	AEFX_CLR_STRUCT(def);
	PF_ADD_POINT_PRECONVERTED("点 4 位置", FLOAT2FIX(45), FLOAT2FIX(45), 0, AF_POS4);

	AEFX_CLR_STRUCT(def);
	PF_ADD_POINT_PRECONVERTED("点 5 位置", FLOAT2FIX(45), FLOAT2FIX(45), 0, AF_POS5);

	AEFX_CLR_STRUCT(def);
	// 设计"延迟（帧）" (): 0-100 滑块, 默认 0;  gating = 延迟/100 ≤ p01
	PF_ADD_SLIDER("点延迟 (帧)", 0, 100, 0, 100, 0, AF_DELAY);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("生长速度 (每秒)", 1.0f, 500.0f, 1.0f, 500.0f, 100.0f, 1, 0, 0, AF_SPEED);

	AEFX_CLR_STRUCT(def);
	// 设计 6 项 (实证 ): 无|在边界附近速度较慢|自定义图层|基于形状的流动|湍流噪波|都
	// 默认 "基于形状的流动" (截图 OCR 值; 实现默认即此)
	PF_ADD_POPUPX("速度图模式", 6, 3, "无|在边界附近速度较慢|自定义图层|基于形状的流动|湍流噪波|都", 0, AF_SMAP_MODE);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("速度图影响", 0.0f, 1.0f, 0.0f, 1.0f, 0.5f, 3, 0, 0, AF_SMAP_INFLUENCE);

	AEFX_CLR_STRUCT(def);
	// 设计仅 亮度|Alpha 2 项 (实证 ) — 实现原 Luma|R|G|B|A 5 项是自行扩展, 与原件不符
	PF_ADD_POPUPX("通道", 2, 0, "亮度|Alpha", 0, AF_CHANNEL);

	AEFX_CLR_STRUCT(def);
	// 默认 0 (截图 100% 是用户设置; 强度>0 时 borderControl 用 Sobel 边缘调制,
	// 内部 0 会压黑填充区 — 需配合边框扩展或设计语义)
	PF_ADD_FLOAT_SLIDERX("边框强度", 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 3, 0, 0, AF_BORDER_STRENGTH);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("边框扩展", 0.0f, 100.0f, 0.0f, 100.0f, 0.0f, 1, 0, 0, AF_BORDER_EXPAND);

	AEFX_CLR_STRUCT(def);
	// 设计 无|遮罩|图层 (实证 ); 渲染侧 bridge=0 (无) 与验证一致
	PF_ADD_POPUPX("桥接模式", 3, 0, "无|遮罩|图层", 0, AF_BRIDGE_MODE);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("桥接厚度", 0.0f, 100.0f, 0.0f, 100.0f, 5.0f, 1, 0, 0, AF_BRIDGE_THICK);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("模糊半径", 0.0f, 200.0f, 0.0f, 200.0f, 0.0f, 1, 0, 0, AF_BLUR_RADIUS);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("曝光", 0.0f, 4.0f, 0.0f, 4.0f, 1.0f, 3, 0, 0, AF_EXPOSURE);

	// ---- Pipeline 组头已移除 (折叠组回退) ----
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("时长 (秒, 0=预设)", 0.0f, 10.0f, 0.0f, 10.0f, 0.0f, 2, 0, 0, AF_DURATION);

	AEFX_CLR_STRUCT(def);
	PF_ADD_SLIDER("重复 (0=预设)", 0, 100, 0, 100, 0, AF_REPEAT);

	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUPX("叠于原图", 3, 0, "自动|叠于原图|替换", 0, AF_COMPOSITE);

	// ---- Pipeline: 渲染器 / 调试视图 ----
	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUPX("渲染器", 3, 0, "CPU|GPU|自动", 0, AF_RENDERER);

	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUPX("调试视图", 4, 0, "关|生长|速度图|边框", 0, AF_VIEW);

	AEFX_CLR_STRUCT(def);
	PF_ADD_BUTTON("关于...", "关于", 0, 0, AF_ABOUT);

	// ---- 追加参数 (注册在尾部: 索引与枚举一致, 旧工程参数值不错位) ----
	AEFX_CLR_STRUCT(def);
	// 设计 Growth Source (实证 ): 点|噪波|图层
	PF_ADD_POPUPX("生长来源", 3, 0, "点|噪波|图层", 0, AF_GROWTH_SOURCE);

	AEFX_CLR_STRUCT(def);
	// 设计"叠加模式"下拉框 ( 原样, 含 5 个 "(-" 分隔符与两个"叠加":
	//   第 1 个"叠加"=设计英文 Add(相加), 第 2 个=Overlay(叠加) — 本地化同词):
	//   1-based 值 →  跳表 → PF_Xfer (A 级依据, 见 dissolve_styles.h 注释):
	//   1 正常 | 3 正片叠底 | 4 颜色加深 | 6 叠加(相加) | 7 滤色 | 9 叠加 |
	//   10 柔光 | 12 颜色 | 14 模版Alpha | 15 模版亮度 | 16 剪影Alpha | 17 剪影亮度
	PF_ADD_POPUPX("混合模式", 17, 1, "正常|(-|正片叠底|颜色加深|(-|叠加|滤色|(-|叠加|柔光|(-|颜色|(-|模版Alpha|模版亮度|剪影Alpha|剪影亮度", 0, AF_BLEND_MODE);

	// ---- 追加: 点 2..5 独立延迟 (设计 param 32/34/36/38, 0-100 帧, 默认 0;
	//      与设计每层延迟对齐; 点 1 延迟=AF_DELAY) ----
	for (int i = 0; i < 4; i++) {
		char name[32];
		snprintf(name, sizeof(name), "点 %d 延迟 (帧)", i + 2);
		AEFX_CLR_STRUCT(def);
		PF_ADD_SLIDER(name, 0, 100, 0, 100, 0, AF_DELAY2 + i);
	}

	// ---- 循环淡出 (0=自动 100%, 1-100=强度): 周期末尾 85%→100% 平滑淡出填充强度。
	//      0=自动: 旧工程无此参数时 AE 迁移值为 0 — 按 100% 处理 (淡出默认开启)。 [B]
	//      修正 [2026-08-16]: 原用 PF_ADD_SLIDER (整数 sd) 但 readParams 读 u.fs_d —
	//      union 错位导致恒读 0 (探针实测 setValue(55) 后插件读到 0) — 改浮点滑块。
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("循环淡出 (0=自动)", 0.0f, 100.0f, 0.0f, 100.0f, 100.0f, 1, 0, 0, AF_LOOP_FADE);

	// ---- 文字模式 (popup, DFLT=2 默认第 2 项"填充覆盖"): 0=设计 1=填充覆盖文字。
	//      设计预设含 mode-3"原图"层: 源图 src-over 盖在填充上 → 文字保持原色,
	//      彩色只填透明背景 (用户三次反馈"蓝块在图层底下" — 默认直接给"填充覆盖",
	//      颜色覆盖文字区域; 切回"设计"可还设计 Reveal 语义)。 [B]
	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUPX("文字模式", 2, 2, "设计|填充覆盖", 0, AF_SOURCE_MODE);

	// 注: 不在 ParamsSetup 里遍历 params[] 加 SUPERVISE —
	// PARAMS_SETUP 期间 params[] 数组大小 = 旧注册数 (首次应用仅输入槽),
	// 按 NUM_PARAMS 遍历会越界 (SEH, cmd=4 崩溃实证 2026-08-16)。
	// SUPERVISE 仅影响 USER_CHANGED_PARAM 回调, 不影响重渲染 — 不加也无碍。

	out_data->num_params = NUM_PARAMS;
	return err;
}

// ---------- 像素格式转换 (depth: 8/16/32 bpc) ----------
// 读取 src world 到 out 缓冲 (out 尺寸 = ow x oh, 通常=output 尺寸);
// src 可能比 out 小 (区域渲染) — 内容放到 (offX, offY) 偏移处 (AE 坐标系:
// src 内容位于 output 的 (src.origin - out.origin) 处), 其余填 0。
// 关键: 只读取 extent_hint 有效区域 (AE 只保证该矩形内数据有效, 区域外可能是
// 垃圾数据 — 忽略它会导致 alpha 哈希漂移 (缓存失效) 和形状错误)。
// 设计语义: 效果限定在图层区域, 不拉伸。
static void copyToFloat(const PF_EffectWorld& w, short depth, std::vector<float>& out,
                        int ow, int oh, int offX, int offY)
{
	size_t n = (size_t)ow * oh;
	out.assign(n * 4, 0.f);
	int sw = (int)w.width, sh = (int)w.height;
	// 有效区域 (extent_hint): 只读该矩形; 无效 (未初始化) 则全图
	int x0 = w.extent_hint.left,   y0 = w.extent_hint.top;
	int x1 = w.extent_hint.right,  y1 = w.extent_hint.bottom;
	if (x1 <= x0 || y1 <= y0) { x0 = 0; y0 = 0; x1 = sw; y1 = sh; }
	x0 = std::max(x0, 0); y0 = std::max(y0, 0);
	x1 = std::min(x1, sw); y1 = std::min(y1, sh);
	const char* src = (const char*)w.data;
	ptrdiff_t rowbytes = w.rowbytes;

	if (depth == 32) {
		for (int y = y0; y < y1; y++) {
			const PF_PixelFloat* row = (const PF_PixelFloat*)(src + (ptrdiff_t)y * rowbytes);
			int oy = y + offY;
			if (oy < 0 || oy >= oh) continue;
			for (int x = x0; x < x1; x++) {
				int ox = x + offX;
				if (ox < 0 || ox >= ow) continue;
				size_t i = (size_t)oy * ow + ox;
				out[i*4+0] = row[x].red; out[i*4+1] = row[x].green;
				out[i*4+2] = row[x].blue; out[i*4+3] = row[x].alpha;
			}
		}
	} else if (depth == 16) {
		for (int y = y0; y < y1; y++) {
			const PF_Pixel16* row = (const PF_Pixel16*)(src + (ptrdiff_t)y * rowbytes);
			int oy = y + offY;
			if (oy < 0 || oy >= oh) continue;
			for (int x = x0; x < x1; x++) {
				int ox = x + offX;
				if (ox < 0 || ox >= ow) continue;
				size_t i = (size_t)oy * ow + ox;
				out[i*4+0] = row[x].red / 32768.0f; out[i*4+1] = row[x].green / 32768.0f;
				out[i*4+2] = row[x].blue / 32768.0f; out[i*4+3] = row[x].alpha / 32768.0f;
			}
		}
	} else {
		for (int y = y0; y < y1; y++) {
			const PF_Pixel8* row = (const PF_Pixel8*)(src + (ptrdiff_t)y * rowbytes);
			int oy = y + offY;
			if (oy < 0 || oy >= oh) continue;
			for (int x = x0; x < x1; x++) {
				int ox = x + offX;
				if (ox < 0 || ox >= ow) continue;
				size_t i = (size_t)oy * ow + ox;
				out[i*4+0] = row[x].red / 255.0f; out[i*4+1] = row[x].green / 255.0f;
				out[i*4+2] = row[x].blue / 255.0f; out[i*4+3] = row[x].alpha / 255.0f;
			}
		}
	}
}

// rw/rh: 渲染缓冲尺寸 (min(src, output)); output 可能比渲染缓冲大 (小图层放大到大 comp)
// 安全: 按 output 尺寸写满, 但只读 in 的 [0,rw)x[0,rh), 越界区域填 0
static void copyFromFloat(const std::vector<float>& in, short depth, PF_EffectWorld& w,
                          int rw, int rh)
{
	const char* dst = (const char*)w.data;
	ptrdiff_t rowbytes = w.rowbytes;
	int ow = (int)w.width, oh = (int)w.height;
	if (rw <= 0 || rh <= 0) rw = ow, rh = oh;
	int copyW = std::min(ow, rw);   // 每行可读的像素数
	int copyH = std::min(oh, rh);   // 可读的行数
	if (depth == 32) {
		for (int y = 0; y < oh; y++) {
			PF_PixelFloat* row = (PF_PixelFloat*)(dst + (ptrdiff_t)y * rowbytes);
			if (y >= copyH) {  // output 比渲染缓冲高: 填透明黑
				for (int x = 0; x < ow; x++)
					row[x].red = row[x].green = row[x].blue = row[x].alpha = 0.f;
				continue;
			}
			for (int x = 0; x < ow; x++) {
				if (x >= copyW) { row[x].red = row[x].green = row[x].blue = row[x].alpha = 0.f; continue; }
				size_t i = (size_t)y * rw + x;
				row[x].red = in[i*4+0]; row[x].green = in[i*4+1];
				row[x].blue = in[i*4+2]; row[x].alpha = in[i*4+3];
			}
		}
	} else if (depth == 16) {
		for (int y = 0; y < oh; y++) {
			PF_Pixel16* row = (PF_Pixel16*)(dst + (ptrdiff_t)y * rowbytes);
			if (y >= copyH) {
				for (int x = 0; x < ow; x++)
					row[x].red = row[x].green = row[x].blue = row[x].alpha = 0;
				continue;
			}
			for (int x = 0; x < ow; x++) {
				if (x >= copyW) { row[x].red = row[x].green = row[x].blue = row[x].alpha = 0; continue; }
				size_t i = (size_t)y * rw + x;
				row[x].red = (A_u_short)(in[i*4+0] * 32768.0f);
				row[x].green = (A_u_short)(in[i*4+1] * 32768.0f);
				row[x].blue = (A_u_short)(in[i*4+2] * 32768.0f);
				row[x].alpha = (A_u_short)(in[i*4+3] * 32768.0f);
			}
		}
	} else {
		for (int y = 0; y < oh; y++) {
			PF_Pixel8* row = (PF_Pixel8*)(dst + (ptrdiff_t)y * rowbytes);
			if (y >= copyH) {
				for (int x = 0; x < ow; x++)
					row[x].red = row[x].green = row[x].blue = row[x].alpha = 0;
				continue;
			}
			for (int x = 0; x < ow; x++) {
				if (x >= copyW) { row[x].red = row[x].green = row[x].blue = row[x].alpha = 0; continue; }
				size_t i = (size_t)y * rw + x;
				row[x].red = (A_u_char)(in[i*4+0] * 255.0f);
				row[x].green = (A_u_char)(in[i*4+1] * 255.0f);
				row[x].blue = (A_u_char)(in[i*4+2] * 255.0f);
				row[x].alpha = (A_u_char)(in[i*4+3] * 255.0f);
			}
		}
	}
}

// ---------- 圆点笔刷 UI 参数 (设计 : 用户放置点, 最多 5 点) ----------
struct BrushUI {
	bool  enable = true;        // 点数>=1 即启用 (设计默认点种子)
	float radius = 10.f;
	int   num = 1;              // 1-5
	float px[10] = { 45.f };    // 5 个点 (x,y) 图层百分比坐标 (设计默认 {45,45}%)
	float delay[5] = { 0.f };   // 每点延迟 (帧 0-100, 层 gating 阈值 = 值/100 ≤ p01)
	float speed = 100.f;        // 生长速度 (per sec, 100=1x)
};

// ---------- 预设属性覆盖 (设计 Pipeline: Duration/Repeat/Composite Over Original) ----------
struct PresetOverride {
	float duration = 0.f;   // >0 时覆盖预设 duration
	int   repeat   = 0;     // >0 时覆盖预设 repeat
	int   composite = 0;    // 0=自动(用预设) 1=叠 2=替换
	float loopFade = 100.f; // 循环淡出强度 0-100% (0=硬回绕)
};

// ---------- 静态场缓存 (见下方 StaticFieldCache; 旧 g_cache* 已移除) ----------

static unsigned long long fingerPrint(const float* alpha, int w, int h) {
	unsigned long long f = 1469598103934665603ULL;
	for (int y = 0; y < h; y += 8)
		for (int x = 0; x < w; x += 8) {
			unsigned v; std::memcpy(&v, &alpha[(size_t)y*w+x], 4);
			f ^= v; f *= 1099511628211ULL;
		}
	f ^= (unsigned long long)w * 0x9E3779B97F4A7C15ULL;
	f ^= (unsigned long long)h * 0xBF58476D1CE4E5B9ULL;
	return f;
}

// ---------- 静态场缓存 (设计组件名 noiseMap.cache/fillMap.cache 同款语义) ----------
// 噪声场/边缘场只依赖 (尺寸, 图层形状), 不依赖时间/进度。
// JFA 距离场不再计算 (实现管线用 BFS 切比雪夫距离, distField 无消费点 —  §三.3)。
// AE 多线程渲染: 用互斥锁保护, 哈希校验形状变化。
struct StaticFieldCache {
	int w = 0, h = 0;
	unsigned long long shapeHash = 0;
	std::vector<float> noiseMap, edgeMap;
};
static StaticFieldCache g_fieldCache;
static std::mutex g_fieldCacheMutex;

static unsigned long long hashAlpha(const float* a, size_t n) {
	// 采样哈希 (每 4 像素取 1): 2M 像素全哈希 ~50ms, 采样版 ~12ms
	// 形状微小变化仍会反映在采样点上 (4 像素间隔足以捕获轮廓位移)
	unsigned long long h = 1469598103934665603ULL;
	size_t stride = 4;
	for (size_t i = 0; i < n; i += stride) {
		unsigned v;
		memcpy(&v, &a[i], 4);
		h ^= v; h *= 1099511628211ULL;
	}
	// 末尾补一个防止 n 整除 stride 时碰撞
	if (n > 0) {
		unsigned v;
		memcpy(&v, &a[n-1], 4);
		h ^= v; h *= 1099511628211ULL;
	}
	return h;
}

// 计算或复用静态场 (buf 输出); 返回 true=本次重算 false=命中缓存
static bool getStaticFields(dissolve::Params& p, dissolve::Buffers& buf,
                            const float* alpha, int w, int h)
{
	size_t n = (size_t)w * h;
	unsigned long long hh = hashAlpha(alpha, n);
	std::lock_guard<std::mutex> lk(g_fieldCacheMutex);
	if (g_fieldCache.w == w && g_fieldCache.h == h &&
	    g_fieldCache.shapeHash == hh &&
	    g_fieldCache.noiseMap.size() == n) {
		buf.noiseMap = g_fieldCache.noiseMap;
		buf.edgeMap  = g_fieldCache.edgeMap;
		buf.shapeHash = hh;
		return false;
	}
	dissolve::generateNoiseMap(p, buf, w, h);
	dissolve::sobelEdges(alpha, buf, w, h);
	g_fieldCache.w = w; g_fieldCache.h = h; g_fieldCache.shapeHash = hh;
	g_fieldCache.noiseMap = buf.noiseMap;
	g_fieldCache.edgeMap  = buf.edgeMap;
	buf.shapeHash = hh;
	return true;
}

// ---------- 核心渲染 (world 级, 传统 Render 与 SmartRender 共用) ----------
static PF_Err
renderWorld(PF_InData* in_data, PF_OutData* out_data,
            PF_EffectWorld* src, PF_EffectWorld* output,
            dissolve::Params& p, int presetIdx, int renderer,
            const BrushUI& brush, const PresetOverride& po, int viewMode,
            A_long current_time, A_u_long time_scale)
{
	PF_Err err = PF_Err_NONE;
	if (presetIdx < 0 || presetIdx >= kNumPresets) presetIdx = 0;

	// 防御: world 数据/尺寸检查 (SmartRender 区域渲染时 src/output 尺寸可能不同)
	if (!src || !output || !src->data || !output->data) {
		logMsg("  renderWorld: null world (src=%p out=%p)", (void*)src, (void*)output);
		return PF_Err_BAD_CALLBACK_PARAM;
	}
	// 渲染基准 = output 尺寸 (src 左上角对齐读取, copyToFloat 处理尺寸差)
	int w = (int)output->width;
	int h = (int)output->height;
	if (w <= 0 || h <= 0) {
		logMsg("  renderWorld: zero size src=%dx%d out=%dx%d",
			(int)src->width, (int)src->height, (int)output->width, (int)output->height);
		return PF_Err_NONE;
	}

	// 像素深度 (8/16/32 bpc)
	AEFX_SuiteScoper<PF_WorldSuite2> wsP = AEFX_SuiteScoper<PF_WorldSuite2>(
		in_data, kPFWorldSuite, kPFWorldSuiteVersion2, out_data);
	PF_PixelFormat fmt = PF_PixelFormat_INVALID;
	if (!wsP->PF_GetPixelFormat) {   // scoper 为 null 时此处崩溃由外层 SEH 兜底并记日志
		logMsg("  renderWorld: WorldSuite2 unavailable");
		return PF_Err_INTERNAL_STRUCT_DAMAGED;
	}
	wsP->PF_GetPixelFormat(src, &fmt);
	short depth = (fmt == PF_PixelFormat_ARGB128) ? 32
	            : (fmt == PF_PixelFormat_ARGB64  ? 16 : 8);
	logFrame("  renderWorld: %dx%d depth=%d fmt=0x%X preset=%d renderer=%d t=%ld/%lu src=%dx%d out=%dx%d",
	       w, h, (int)depth, (unsigned)fmt, presetIdx, renderer,
	       (long)current_time, (unsigned long)time_scale,
	       (int)src->width, (int)src->height, (int)output->width, (int)output->height);
	// 坐标系诊断: AE 区域渲染时 world 带 origin (合成坐标偏移), 忽略会导致内容偏移
	logFrame("  origin: src=(%d,%d) out=(%d,%d) srcRect=(%d,%d,%d,%d) outRect=(%d,%d,%d,%d)",
	       (int)src->origin_x, (int)src->origin_y, (int)output->origin_x, (int)output->origin_y,
	       (int)src->extent_hint.left, (int)src->extent_hint.top,
	       (int)src->extent_hint.right, (int)src->extent_hint.bottom,
	       (int)output->extent_hint.left, (int)output->extent_hint.top,
	       (int)output->extent_hint.right, (int)output->extent_hint.bottom);
	logMsg("  [src] growthSource=%d numPts=%d radius=%.0f speed=%.0f delay=%.1f smapMode=%d",
	       p.growthSource, brush.num, brush.radius, brush.speed, brush.delay[0], p.speedMapMode);

	// 阶段计时 (前 60 帧全记录, 定位性能瓶颈)
	LARGE_INTEGER t0, t1, t2, t3, t4, t5, freq;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);
	static long g_timed = 0;
	long timedN = InterlockedIncrement(&g_timed);
	bool doTiming = (timedN <= 60);
#define TIMER_MARK(name) \
	if (doTiming) { QueryPerformanceCounter(&t1); \
		logMsg("  [timing] %s: %.2f ms", name, \
			(double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart); \
		t0 = t1; }

	std::vector<float> rgba;
	copyToFloat(*src, depth, rgba, w, h,
	            (int)src->origin_x - (int)output->origin_x,
	            (int)src->origin_y - (int)output->origin_y);
	TIMER_MARK("copyToFloat");

	// ---- 预设多层渲染管线 (CPU/GPU 双路径) ----
	dissolve::Buffers buf;
	Preset pres = kPresets[presetIdx];
	// 面板覆盖预设属性 (设计 Pipeline 参数, 0/自动 = 用预设)
	// 默认演化周期 3s 全程循环: 预设 duration=1s 在 43.5s 合成里只演 1 秒,
	// 合成时长慢扫又观感静止 — 3s 循环保证任何时刻都在演化 [B]。
	// AF_DURATION/AF_REPEAT >0 可精确覆盖。
	{
		float compDur = (float)std::max<A_long>(in_data->total_time, 1) /
		                (float)std::max<A_u_long>(time_scale, 1);
		float dur = 3.0f;
		if (po.duration > 0.f) dur = po.duration;
		int rep = (po.repeat > 0) ? po.repeat
		                          : std::max(1, (int)std::ceil(compDur / dur));
		pres.duration = std::max(dur, 0.1f);
		pres.repeat = rep;
		logMsg("  [dur] comp=%.1fs cycle=%.1fs repeat=%d", compDur, dur, rep);
	}
	if (po.composite > 0) pres.compOverOriginal = po.composite - 1;

	// 2. 时间进度 (播放头) — 设计 A 级公式 (实现-参数来源.md §1.2):
	//   frame01 = (int)低32位(time) / (int)高32位(time)  = A_Time value/scale = 秒
	//   v1 (项目版本 v1): divisor = param17 (持续时间帧)
	//   v2:                divisor = 帧 / 高32位 × param18 (持续时间秒)
	//   p01 = clamp((frame01 + 1) / divisor, 0, 1)   ← +1: 首帧即有内容; 无循环, 到 1 停住
	// 实现映射 [C]: divisor = pres.duration × 帧率 (param17 "持续时间帧" 同量纲;
	//   分子 = 已过帧数+1, 首帧 1/总帧数 ≈ 3% 进度, duration 秒完成)
	// 起始可见度 [B]: 设计 +1 帧在 4000×4000 合成下 t=0 波前半径 ≈ 0 (1/90 ≈ 1.1%),
	//   打开工程时效果完全不可见 (日志实证 t=0 lit=2416/16M)。加起始偏置 kStartBias:
	//   t=0 波前即有 12% 半径 — 打开即见效果, 循环首尾也连续。
	const float kStartBias = 0.12f;
	float seconds = (float)current_time / (float)std::max<A_u_long>(time_scale, 1);
	// 循环演化: repeat>1 时时间折叠进周期 (波前周期性扫过, 任何时刻可见动画)
	if (pres.repeat > 1)
		seconds = std::fmod(seconds, pres.duration);
	// 设计进度: p01 = clamp((帧号+1)/divisor, 0, 1) — 不含全局延迟偏移;
	// 延迟(帧) = 每层 gating 阈值 (: 延迟/100 ≤ p01), 在 renderPresetDirect 内消费
	float speedF = std::max(brush.speed, 1.f) / 100.f;    // 100 = 1x
	float fps = (float)std::max<A_u_long>(time_scale, 1) / 1024.f;
	float divisor = std::max(pres.duration * fps, 1.f);
	float p01 = std::min(std::max((seconds * speedF * fps + kStartBias * divisor) / divisor, 0.f), 1.f);
	float prog = p01 * 100.f;
	logMsg("  [time] sec=%.2f cycle=%.1fs rep=%d prog=%.1f%%", seconds, pres.duration, pres.repeat, prog);

	// 源 alpha (最终合成需要; GPU 路径也用它)
	std::vector<float> alpha((size_t)w*h);
	for (size_t i = 0; i < (size_t)w*h; i++) alpha[i] = rgba[i*4+3];

	// 3. 图层渲染: 实现管线 (GrowthDrawCPU ); GPU 已禁用
	std::vector<float> cR, cG, cB, cA;
	bool useGPU = false;
	// GPU 路径已禁用 (日志实证 2026-08-15):
	//   AE 内隐藏窗口 GL 上下文与 AE 自身 GPU 管线冲突 — renderFrame 实测 6.8-7.1s/帧
	//   (makeCurrent 切换触发驱动同步), 且输出时黑时好。CPU 路径稳定。
	// 渲染器参数保留 UI (对齐设计), 但渲染统一走 CPU。
	if (renderer == 1 || renderer == 2)
		logMsg("  [diag] GPU 禁用 (renderer=%d), 走 CPU", renderer);
	// GPU 分支保留但永远不执行 (useGPU 恒 false); seedMask 由实现管线替代
	const float* seedMask = nullptr;
	if (useGPU) {
		// 噪声参数数组 (glr 需要; 顺序: noiseScale/scaleX/scaleY/brightness/contrast/evolution/aspect)
		float noiseParams[7] = {
			p.noiseScale, p.scaleX, p.scaleY,
			p.brightness, p.contrast, p.evolution, 1.0f
		};
		if (glr::renderFrame(rgba.data(), w, h, pres, prog,
		                     noiseParams, p.complexityL, p.alphaThreshold,
		                     p.speedMapInfluenceF, p.borderInfluenceF,
		                     p.gammaF, p.exposureF, p.speedMapChannel, cA, seedMask,
		                     p.blendMode)) {
			// GPU 路径: cA 是 RGBA (图层合成结果), 拆通道
			cR.assign((size_t)w*h, 0.f);
			cG.assign((size_t)w*h, 0.f);
			cB.assign((size_t)w*h, 0.f);
			std::vector<float> layerA((size_t)w*h);
			for (int i = 0; i < w*h; i++) {
				cR[i] = cA[i*4+0]; cG[i] = cA[i*4+1];
				cB[i] = cA[i*4+2]; layerA[i] = cA[i*4+3];
			}
			cA.swap(layerA);
		} else {
			useGPU = false;  // GPU 失败回退 CPU
		}
	}
	if (!useGPU) {
		// ---- 渲染区域 (extent): 内容包围盒, 区域外透明不渲染 ----
		// 用户图层 64x64 (内容 23x7) 被 AE 放大到 400 渲染 → 全图渲染 420ms+7s 重算
		// extent 区域渲染: 管线只在内容矩形上跑 (23x7 → <0.1ms)
		int offX = (int)src->origin_x - (int)output->origin_x;
		int offY = (int)src->origin_y - (int)output->origin_y;
		int rx0 = src->extent_hint.left + offX, ry0 = src->extent_hint.top + offY;
		int rx1 = src->extent_hint.right + offX, ry1 = src->extent_hint.bottom + offY;
		rx0 = std::max(rx0, 0); ry0 = std::max(ry0, 0);
		rx1 = std::min(rx1, w); ry1 = std::min(ry1, h);
		bool useRect = (rx1 > rx0 && ry1 > ry0);
		// 点传播可能溢出 extent (点 45,45 在内容外) — 区域外扩传播余量
		// (设计点从任意位置向内容传播; 溢出部分显示在图层透明区, 不影响内容)
		int rw = w, rh = h, baseX = 0, baseY = 0;
		if (useRect) {
			rw = rx1 - rx0; rh = ry1 - ry0; baseX = rx0; baseY = ry0;
		}
		// 区域 rgba/alpha 提取
		std::vector<float> rgbaR((size_t)rw * rh * 4, 0.f);
		std::vector<float> alphaR((size_t)rw * rh, 0.f);
		for (int y = 0; y < rh; y++)
			for (int x = 0; x < rw; x++) {
				size_t di = (size_t)y * rw + x;
				size_t si = (size_t)(y + baseY) * w + (x + baseX);
				rgbaR[di*4+0] = rgba[si*4+0]; rgbaR[di*4+1] = rgba[si*4+1];
				rgbaR[di*4+2] = rgba[si*4+2]; rgbaR[di*4+3] = rgba[si*4+3];
				alphaR[di] = alpha[si];
			}
		// 1. 基础场: 噪声 + 边缘 + 距离场 (静态, 缓存复用; 设计 noiseMap.cache 语义)
		bool recomputed = getStaticFields(p, buf, alphaR.data(), rw, rh);
		if (recomputed) logFrame("  [cache] 静态场重算 (%dx%d)", rw, rh);
		TIMER_MARK("staticFields(缓存/重算)");
		// CPU 路径: 实现管线 (GrowthDrawCPU  主流程)
		dissolve::DirectFrame dfr;
		dfr.preset = &pres;
		dfr.progress01 = std::min(std::max(prog / 100.f, 0.f), 1.f);
		float fps = (float)std::max<A_u_long>(time_scale, 1) / 1024.f;
		dfr.totalFrames = std::max(pres.duration * fps, 1.f);
		// 已过帧数 = 秒×帧率 (fillMap 膨胀帧号); + kStartBias×totalFrames:
		// t=0 波前半径 = 12% 距离跨度 (与 p01 偏置一致, 打开即见) [B]
		dfr.explicitFrames = seconds * speedF * fps + kStartBias * dfr.totalFrames;
		// 循环包络 [B]: 周期末尾 (85%→100%) 平滑淡出, 消除 fmod 回绕的硬跳变
		// ("满填充突然消失"); 源图 (mode-3) 层由 renderPresetDirect 豁免 (文字不闪)。
		// 强度由面板"循环淡出"参数控制 (0=自动→100)。
		float fadeF = std::min(std::max(po.loopFade, 0.f), 100.f) * 0.01f;
		dfr.loopEnv = 1.f - fadeF * dissolve::smoothstepField(0.85f, 1.f,
			std::min(std::max(p01, 0.f), 1.f));
		logMsg("  [env] p01=%.3f fadeF=%.2f loopEnv=%.3f", p01, fadeF, dfr.loopEnv);
		dfr.splatRadius = brush.radius;   // param 28 半径 (默认 10)
		dfr.rampS = 1.f;                  // param 24 噪波对比度 [C 默认]
		{  // param 6 质量 → 除数表 {1,1,2,0.5} (A 级, )
			static const float kDivTable[4] = {1.f, 1.f, 2.f, 0.5f};
			dfr.divisor = std::max(kDivTable[std::min(std::max(p.quality, 0), 3)], 0.001f);
		}
		dfr.ox = 0.f; dfr.oy = 0.f;
		dfr.growthSource = p.growthSource;  // param 9 生长来源: 0=点 1=噪波 2=图层
		dfr.sourceLayerMode = p.sourceMode;  // 文字模式: 1=跳过 mode-3 源图层 (填充覆盖文字)
		dfr.srcRGBA = rgbaR.data();
		dfr.noiseFill = buf.noiseMap.empty() ? nullptr : buf.noiseMap.data();
		dfr.shapeAlpha = alphaR.data();  // 传播/填充限定在图层内容形状 (dilate cull 语义)
		// 层点/阈值: 设计参数 29+2i (点位置 16.16 定点, 默认 {45,45}) / 30+2i (延迟, 0-100)
		// 点坐标换算 [B 修正]: 设计 cx = x×2⁻¹⁶÷除数 − ox, 点 = 图层尺寸百分比;
		//   传统 Render 下 world==图层 → 锚定世界即锚定图层。但矢量文字层在合成
		//   分辨率下渲染, world=合成尺寸 (4000×4000), 图层≈内容包围盒 (extent):
		//   锚定世界会使种子随图层位置漂移 — 用户实证"只有拖动图层才有一点画面变化"
		//   (拖动→extent/原点变化→种子相对文字移动→波前图案改变)。
		//   修正: 锚定到内容包围盒 (区域) — 拖动图层不再改变波前相对文字的位置。
		//   区域外钳制保留 (极端百分比值, 保证种子可见)。
		int nL = std::min(pres.nLayers, 5);
		std::vector<float> dPts((size_t)nL * 2, 0.f), dTh(nL, 0.f);
		for (int li = 0; li < nL; li++) {
			float pxPct = brush.px[(size_t)li*2+0];   // 45.0 = 45% (FIX_2_FLOAT)
			float pyPct = brush.px[(size_t)li*2+1];
			float wx = (float)rw * (pxPct / 100.f);
			float wy = (float)rh * (pyPct / 100.f);
			// 钳制进区域 (留半个半径余量)
			float r = std::max(brush.radius, 1.f) * 0.5f;
			wx = std::min(std::max(wx, r), (float)rw - r);
			wy = std::min(std::max(wy, r), (float)rh - r);
			dPts[(size_t)li*2+0] = wx;
			dPts[(size_t)li*2+1] = wy;
			// 层 gating 阈值 = 每层延迟参数 30+2i (0-100 帧, 默认 0, A 级):
			//    硬阈值 gating = 延迟/100 ≤ p01
			dTh[li] = std::min(std::max(brush.delay[li] / 100.f, 0.f), 1.f);
		}
		if (nL > 0) {
			int sx = std::min(std::max((int)dPts[0], 0), rw - 1);
			int sy = std::min(std::max((int)dPts[1], 0), rh - 1);
			float aSeed = alphaR[(size_t)sy * rw + sx];
			logMsg("  [seed] n=%d p0=(%.1f,%.1f) region=%dx%d base=(%d,%d) alphaAtSeed=%.2f",
				nL, dPts[0], dPts[1], rw, rh, baseX, baseY, aSeed);
		}
		dfr.layerPts = dPts.data();
		dfr.layerThresh = dTh.data();
		dfr.blendMode = p.blendMode;  // 面板混合模式 ( 跳表权威映射, 1-based)
		// BFS 距离场缓存键 = 形状指纹 ^ 点/半径配置指纹
		{
			uint64_t k = buf.shapeHash;
			for (int bi = 0; bi < 10; bi++) {
				uint32_t u; std::memcpy(&u, &brush.px[bi], 4);
				k = k * 131ULL + u;
			}
			uint32_t u; std::memcpy(&u, &brush.radius, 4);
			dfr.staticKey = k * 131ULL + u;
		}
		// Fill_GPU (): speedOverlay + borderControl + gamma/exposure
		dissolve::speedMapFromSource(rgbaR.data(), rw, rh,
		                             std::max(p.speedMapMode, 1),
		                             p.speedMapChannel, 1.f, buf.speedMap);
		dfr.speedMap = buf.speedMap.data();
		dfr.speedMapInfluenceF = p.speedMapInfluenceF;
		dfr.edgeMap = buf.edgeMap.empty() ? nullptr : buf.edgeMap.data();
		dfr.borderInfluenceF = p.borderInfluenceF;
		dfr.blurRadius = p.blurRadius;   // fill 链 2/3 步 (param 96 模糊)
		dfr.gammaF = p.gammaF;
		dfr.exposureF = p.exposureF;
		std::vector<float> cRR, cGG, cBB, cAA;
		dissolve::renderPresetDirect(dfr, rw, rh, cRR, cGG, cBB, cAA);
		TIMER_MARK("renderPresetDirect");
		// 区域结果写回全图
		cR.assign((size_t)w * h, 0.f);
		cG.assign((size_t)w * h, 0.f);
		cB.assign((size_t)w * h, 0.f);
		cA.assign((size_t)w * h, 0.f);
		for (int y = 0; y < rh; y++)
			for (int x = 0; x < rw; x++) {
				size_t di = (size_t)y * rw + x;
				size_t si = (size_t)(y + baseY) * w + (x + baseX);
				cR[si] = cRR[di]; cG[si] = cGG[di];
				cB[si] = cBB[di]; cA[si] = cAA[di];
			}
		// 每帧输出诊断 (不降频 — 排查"没有效果")
		{
			size_t lit = 0;
			float mxA = 0.f;
			for (size_t i = 0; i < cA.size(); i++) {
				if (cA[i] > 0.01f) lit++;
				if (cA[i] > mxA) mxA = cA[i];
			}
			logMsg("  [out] prog=%.1f lit=%zu/%zu maxA=%.2f region=%dx%d", prog, lit, cA.size(), mxA, rw, rh);
		}
	}
	logFrame("  renderWorld: useGPU=%d prog=%.1f maxLayerA=%.3f", useGPU, prog,
	       cA.empty() ? -1.f : *std::max_element(cA.begin(), cA.end()));

	// 4. 最终合成: compOverOriginal 决定图层与源图混合
	bool compOver = (pres.compOverOriginal == 0);
	for (int i = 0; i < w*h; i++) {
		float a = cA[i];
		float srcA = alpha[i];
		float outR, outG, outB, outA;
		if (compOver) {
			// 图层叠在源图上 (Reveal: 图层显示在填充区)
			outR = rgba[i*4+0] * (1-a) + cR[i] * a;
			outG = rgba[i*4+1] * (1-a) + cG[i] * a;
			outB = rgba[i*4+2] * (1-a) + cB[i] * a;
			outA = std::max(srcA, a);
		} else {
			// 图层替换源图 (Out: 源图被消耗)
			outR = cR[i] * a + rgba[i*4+0] * (1-a);
			outG = cG[i] * a + rgba[i*4+1] * (1-a);
			outB = cB[i] * a + rgba[i*4+2] * (1-a);
			outA = srcA * (1-a);
		}
		rgba[i*4+0] = outR; rgba[i*4+1] = outG;
		rgba[i*4+2] = outB; rgba[i*4+3] = outA;
	}

	// (调试视图已禁用 — 灰度场输出曾致"全屏黑"误解, 始终输出最终效果)
	(void)viewMode;

	copyFromFloat(rgba, depth, *output, w, h);
	TIMER_MARK("composite+copyFromFloat");
#undef TIMER_MARK
	// 输出抽查: 中心像素 (验证非黑, 降频)
	{
		size_t ci = ((size_t)(h/2) * w + w/2) * 4;
		logFrame("  renderWorld: out center=(%.3f,%.3f,%.3f,%.3f)",
		       rgba[ci+0], rgba[ci+1], rgba[ci+2], rgba[ci+3]);
	}
	return err;
}

// ---------- 参数读取 (传统 Render 与 SmartRender 共用) ----------
static void
readParams(PF_ParamDef* params[], dissolve::Params& p,
           int& presetIdx, int& renderer, BrushUI& brush, int& viewMode,
           PresetOverride& po)
{
	// 内部固定参数 (设计面板未暴露: 噪点/缩放/亮度/对比度/演变/复杂度/阈值/伽马/剔除)
	p.noiseScale = 1.0f;
	p.scaleX = 1.0f;
	p.scaleY = 1.0f;
	p.brightness = 0.5f;
	p.contrast = 1.0f;
	p.evolution = 0.0f;
	p.complexityL = 4;
	p.alphaThreshold = 0.5f;
	p.gammaF = 1.0f;
	p.cullB = 0;
	// 面板参数
	p.numSamples = 8;  // 旧采样数语义 (仅死代码 growthStep 使用, 保留常量)
	p.quality    = params[AF_QUALITY]->u.pd.value;  // param 6: 0=完整 1=一半 2=双重
	p.speedMapInfluenceF = params[AF_SMAP_INFLUENCE]->u.fs_d.value;
	// 设计 6 项 → 内核 mode (实证: 内核仅 {1,2}, 无第三模式):
	//   0 无 → 0 (关闭, influence 置 0)
	//   1 在边界附近速度较慢 → 1 [C 回落]   2 自定义图层 → 1 [C 回落]
	//   3 基于形状的流动 → 1 (实现默认)     4 湍流噪波 → 1 [C 回落]
	//   5 都 → 2 (mode2: 主通道/scale)
	{
		int sm = params[AF_SMAP_MODE]->u.pd.value;
		p.speedMapMode = (sm == 5) ? 2 : 1;
		if (sm == 0) { p.speedMapMode = 0; p.speedMapInfluenceF = 0.f; }
	}
	p.speedMapChannel = params[AF_CHANNEL]->u.pd.value;  // 0=亮度 1=Alpha (设计 2 项)
	p.speedMapScale   = 1.0f;  // : 位深常量 (float 源=1.0)
	p.blurRadius      = params[AF_BLUR_RADIUS]->u.fs_d.value;
	p.borderExpand    = (int)params[AF_BORDER_EXPAND]->u.fs_d.value;
	p.borderInfluenceF   = params[AF_BORDER_STRENGTH]->u.fs_d.value;
	p.exposureF    = params[AF_EXPOSURE]->u.fs_d.value;
	p.timeF        = 1.0f;  // 时间速度固定 (动画由 AF_SPEED 驱动)
	// 生长来源 (设计 param 9, AE popup 1-based: 点=1 噪波=2 图层=3; 内核分派比较 1/2/3)
	// → 实现内部 0=点/1=噪波/2=图层 (S3 独立复验备注: 设计分派比较 1/2/3)
	p.growthSource = std::min(std::max(params[AF_GROWTH_SOURCE]->u.pd.value - 1, 0), 2);
	p.blendMode    = params[AF_BLEND_MODE]->u.pd.value;     // 1-based ( 权威映射)
	// 参数身份诊断: 验证 params[ID] 与 UI 控件是否错位 (旧工程折叠组版本可能污染索引)
	logMsg("  [param] preset=%d renderer=%d growthSource=%d (raw pd=%d id=%d) blend=%d view=%d loopFade=%.0f srcMode=%d(raw %d)",
	       presetIdx, renderer, p.growthSource,
	       (int)params[AF_GROWTH_SOURCE]->u.pd.value, (int)params[AF_GROWTH_SOURCE]->uu.id,
	       p.blendMode, viewMode,
	       (double)params[AF_LOOP_FADE]->u.fs_d.value,
	       p.sourceMode, (int)params[AF_SOURCE_MODE]->u.pd.value);
	presetIdx  = params[AF_PRESET]->u.pd.value - 2;  // 设计列表: None/Reset 前置 (32 项)
	renderer   = params[AF_RENDERER]->u.pd.value;
	// 圆点笔刷 (5 点) — 旧工程参数损坏实证: 点值 = 越界垃圾 (671%/1737% 等),
	// 且损坏的工程参数状态会使 AE 对效果实例的时间求值异常 (每帧 current_time=0)。
	// 自愈: 越界值写回默认 45 (设计 {45,45}%) — 经 params[] change_flags 提交回 AE,
	// 一次修复后工程参数恢复干净。
	brush.enable = true;
	brush.radius = params[AF_RADIUS]->u.fs_d.value;
	brush.num    = params[AF_POINT_COUNT]->u.pd.value + 1;
	// 自愈 [修正 2026-08-16]: 只修内存值, 不再设置 PF_ChangeFlag_CHANGED_VALUE。
	//   写回标记不生效且触发 AE 反复重渲同一帧 (日志实证: 同时间同 base 连续渲染
	//   12+ 次) — 交互预览卡死在当前帧, 播放无新帧 = "正常加载看不到动画"。
	//   工程里的垃圾值每帧在渲染内修正 (10 次浮点比较, 无副作用)。
	static long g_healLogged = 0;
	for (int bi = 0; bi < 10; bi++) {
		PF_ParamDef* pd = nullptr;
		switch (bi / 2) {
		case 0: pd = params[AF_POS1]; break;
		case 1: pd = params[AF_POS2]; break;
		case 2: pd = params[AF_POS3]; break;
		case 3: pd = params[AF_POS4]; break;
		default: pd = params[AF_POS5]; break;
		}
		float v = (bi % 2 == 0) ? FIX_2_FLOAT(pd->u.td.x_value)
		                        : FIX_2_FLOAT(pd->u.td.y_value);
		if (!(v >= 0.f && v <= 100.f)) {
			float rawV = v;
			// 渲染内修正为默认 (16.16 定点 45 = )
			if (bi % 2 == 0) pd->u.td.x_value = 45 << 16;
			else             pd->u.td.y_value = 45 << 16;
			v = 45.f;
			if (InterlockedIncrement(&g_healLogged) <= 20)
				logMsg("  [heal] point %d %c 越界 (%.1f%%) → 45%%", bi / 2 + 1,
				       (bi % 2 == 0) ? 'x' : 'y', rawV);
		}
		brush.px[bi] = v;
	}
	logMsg("  [pts] raw=(%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f)",
	       FIX_2_FLOAT(params[AF_POS1]->u.td.x_value), FIX_2_FLOAT(params[AF_POS1]->u.td.y_value),
	       FIX_2_FLOAT(params[AF_POS2]->u.td.x_value), FIX_2_FLOAT(params[AF_POS2]->u.td.y_value),
	       FIX_2_FLOAT(params[AF_POS3]->u.td.x_value), FIX_2_FLOAT(params[AF_POS3]->u.td.y_value),
	       FIX_2_FLOAT(params[AF_POS4]->u.td.x_value), FIX_2_FLOAT(params[AF_POS4]->u.td.y_value),
	       FIX_2_FLOAT(params[AF_POS5]->u.td.x_value), FIX_2_FLOAT(params[AF_POS5]->u.td.y_value));
	brush.delay[0] = params[AF_DELAY]->u.sd.value;   // 0-100 帧 (设计"延迟（帧）")
	brush.delay[1] = params[AF_DELAY2]->u.sd.value;
	brush.delay[2] = params[AF_DELAY3]->u.sd.value;
	brush.delay[3] = params[AF_DELAY4]->u.sd.value;
	brush.delay[4] = params[AF_DELAY5]->u.sd.value;
	brush.speed  = params[AF_SPEED]->u.fs_d.value;
	// 调试视图
	viewMode = params[AF_VIEW]->u.pd.value;
	// 预设属性覆盖 (0 = 用预设)
	po.duration  = params[AF_DURATION]->u.fs_d.value;
	po.repeat    = params[AF_REPEAT]->u.sd.value;
	po.composite = params[AF_COMPOSITE]->u.pd.value;
	po.loopFade  = params[AF_LOOP_FADE]->u.fs_d.value;
	if (po.loopFade <= 0.f) po.loopFade = 100.f;  // 0=自动 (旧工程迁移值=0)
	p.sourceMode = std::min(std::max(params[AF_SOURCE_MODE]->u.pd.value - 1, 0), 1);
}

// ---------- 渲染异常防御壳 ----------
// 分工: 本函数捕 C++ 异常 (bad_alloc 等, std::vector 展开安全);
//       SEH 访问违例由 EffectMain 的 __except 兜底
static PF_Err
renderWorldSafe(PF_InData* in_data, PF_OutData* out_data,
                PF_EffectWorld* src, PF_EffectWorld* output,
                dissolve::Params& p, int presetIdx, int renderer,
                const BrushUI& brush, const PresetOverride& po, int viewMode,
                A_long current_time, A_u_long time_scale)
{
	PF_Err err = PF_Err_NONE;
	try {
		err = renderWorld(in_data, out_data, src, output, p, presetIdx, renderer, brush, po, viewMode,
		                  current_time, time_scale);
	} catch (...) {
		err = PF_Err_OUT_OF_MEMORY;
	}
	return err;
}

// ---------- 传统渲染路径 ----------
static PF_Err
Render(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
	PF_Err err = PF_Err_NONE;
	dissolve::Params p;
	int presetIdx = 0, renderer = 2, viewMode = 0;
	BrushUI brush;
	PresetOverride po;
	readParams(params, p, presetIdx, renderer, brush, viewMode, po);
	return renderWorldSafe(in_data, out_data, &params[AF_INPUT]->u.ld, output,
	                       p, presetIdx, renderer, brush, po, viewMode,
	                       in_data->current_time, in_data->time_scale);
}

// ---------- 智能渲染: 预渲染 ----------
// 注意: 所有参数在 PreRender 阶段 checkout 并存入 SRData,
//       SmartRender 阶段只读 SRData (SDK 规范, 避免 SmartRender 内 checkout)
typedef struct {
	int presetIdx;
	int renderer;
	int quality;
	float speedInfluence, exposure;
	float borderStrength;
	int speedMapChannel;   // 0=亮度 1=Alpha (设计 2 项)
	int speedMapMode;      // 面板速度图模式索引 (0-5)
	float blurRadius, borderExpand;
	// 预设属性覆盖
	float durOverride;
	int repeatOverride, compositeOverride;
	float loopFade;
	// 圆点笔刷 (5 点 + 每点延迟/速度)
	int pointCount;
	float radius;
	float px[10];
	float delay[5];
	float speed;
	int viewMode;
	int growthSource;
	int blendMode;
	int sourceMode;
} SRData;

// UnionLRect 内联实现 (Smart_Utils.cpp 依赖已移除)
static void unionLRect(const PF_LRect* src, PF_LRect* dst) {
	if (src->left < dst->left) dst->left = src->left;
	if (src->top < dst->top) dst->top = src->top;
	if (src->right > dst->right) dst->right = src->right;
	if (src->bottom > dst->bottom) dst->bottom = src->bottom;
}

static PF_Err
PreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* extra)
{
	PF_Err err = PF_Err_NONE;
	PF_RenderRequest req = extra->input->output_request;
	PF_CheckoutResult in_result;
	AEFX_CLR_STRUCT(in_result);

	// 分配 pre_render_data
	AEFX_SuiteScoper<PF_HandleSuite1> handleSuite =
		AEFX_SuiteScoper<PF_HandleSuite1>(in_data, kPFHandleSuite,
			kPFHandleSuiteVersion1, out_data);
	PF_Handle infoH = handleSuite->host_new_handle(sizeof(SRData));
	if (!infoH) return PF_Err_OUT_OF_MEMORY;

	SRData* info = reinterpret_cast<SRData*>(handleSuite->host_lock_handle(infoH));
	if (!info) { handleSuite->host_dispose_handle(infoH); return PF_Err_OUT_OF_MEMORY; }
	memset(info, 0, sizeof(SRData));

	// checkout 全部渲染参数 (传统路径 readParams 同款)
	PF_ParamDef pdef;
	AEFX_CLR_STRUCT(pdef);
#define CHECKOUT_P(ID, FIELD, TYPE) \
	ERR(PF_CHECKOUT_PARAM(in_data, ID, in_data->current_time, \
		in_data->time_step, in_data->time_scale, &pdef)); \
	if (!err) { info->FIELD = pdef.u.TYPE.value; AEFX_CLR_STRUCT(pdef); }
	CHECKOUT_P(AF_PRESET, presetIdx, pd)
	CHECKOUT_P(AF_RENDERER, renderer, pd)
	CHECKOUT_P(AF_QUALITY, quality, pd)
	CHECKOUT_P(AF_SMAP_INFLUENCE, speedInfluence, fs_d)
	CHECKOUT_P(AF_SMAP_MODE, speedMapMode, pd)
	CHECKOUT_P(AF_BORDER_STRENGTH, borderStrength, fs_d)
	CHECKOUT_P(AF_CHANNEL, speedMapChannel, pd)
	CHECKOUT_P(AF_BLUR_RADIUS, blurRadius, fs_d)
	CHECKOUT_P(AF_BORDER_EXPAND, borderExpand, fs_d)
	CHECKOUT_P(AF_EXPOSURE, exposure, fs_d)
	CHECKOUT_P(AF_DURATION, durOverride, fs_d)
	CHECKOUT_P(AF_REPEAT, repeatOverride, sd)
	CHECKOUT_P(AF_COMPOSITE, compositeOverride, pd)
	CHECKOUT_P(AF_LOOP_FADE, loopFade, fs_d)
	CHECKOUT_P(AF_GROWTH_SOURCE, growthSource, pd)
	info->growthSource = std::min(std::max(info->growthSource - 1, 0), 2);  // AE 1-based → 内部 0/1/2
	CHECKOUT_P(AF_BLEND_MODE, blendMode, pd)
	CHECKOUT_P(AF_VIEW, viewMode, pd)
	CHECKOUT_P(AF_SOURCE_MODE, sourceMode, pd)
	info->sourceMode = std::min(std::max(info->sourceMode - 1, 0), 1);  // 1-based → 0/1
#undef CHECKOUT_P
	// 设计预设列表: None/Reset 前置 (32 项) -> 与 readParams 偏移一致
	info->presetIdx -= 2;
	// 圆点笔刷 (PF_POINT 用 u.td.x_value)
	{
		ERR(PF_CHECKOUT_PARAM(in_data, AF_POINT_COUNT, in_data->current_time,
			in_data->time_step, in_data->time_scale, &pdef));
		if (!err) { info->pointCount = pdef.u.pd.value + 1; AEFX_CLR_STRUCT(pdef); }
		ERR(PF_CHECKOUT_PARAM(in_data, AF_RADIUS, in_data->current_time,
			in_data->time_step, in_data->time_scale, &pdef));
		if (!err) { info->radius = pdef.u.fs_d.value; AEFX_CLR_STRUCT(pdef); }
		const int delayIds[5] = { AF_DELAY, AF_DELAY2, AF_DELAY3, AF_DELAY4, AF_DELAY5 };
		for (int k = 0; k < 5; k++) {
			ERR(PF_CHECKOUT_PARAM(in_data, delayIds[k], in_data->current_time,
				in_data->time_step, in_data->time_scale, &pdef));
			if (!err) { info->delay[k] = pdef.u.sd.value; AEFX_CLR_STRUCT(pdef); }
		}
		ERR(PF_CHECKOUT_PARAM(in_data, AF_SPEED, in_data->current_time,
			in_data->time_step, in_data->time_scale, &pdef));
		if (!err) { info->speed = pdef.u.fs_d.value; AEFX_CLR_STRUCT(pdef); }
		const int posIds[5] = { AF_POS1, AF_POS2, AF_POS3, AF_POS4, AF_POS5 };
		for (int k = 0; k < 5; k++) {
			ERR(PF_CHECKOUT_PARAM(in_data, posIds[k], in_data->current_time,
				in_data->time_step, in_data->time_scale, &pdef));
			if (!err) {
				info->px[k*2]   = FIX_2_FLOAT(pdef.u.td.x_value);
				info->px[k*2+1] = FIX_2_FLOAT(pdef.u.td.y_value);
				AEFX_CLR_STRUCT(pdef);
			}
		}
	}

	// checkout 输入图层
	ERR(extra->cb->checkout_layer(in_data->effect_ref, AF_INPUT, AF_INPUT, &req,
		in_data->current_time, in_data->time_step, in_data->time_scale, &in_result));
	if (!err) {
		unionLRect(&in_result.result_rect, &extra->output->result_rect);
		unionLRect(&in_result.max_result_rect, &extra->output->max_result_rect);
		extra->output->pre_render_data = infoH;
	} else {
		handleSuite->host_dispose_handle(infoH);
		return err;
	}
	handleSuite->host_unlock_handle(infoH);
	return err;
}

// ---------- 智能渲染: 渲染 ----------
static PF_Err
SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extra)
{
	PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
	AEFX_SuiteScoper<PF_HandleSuite1> handleSuite =
		AEFX_SuiteScoper<PF_HandleSuite1>(in_data, kPFHandleSuite,
			kPFHandleSuiteVersion1, out_data);

	PF_Handle infoH = reinterpret_cast<PF_Handle>(extra->input->pre_render_data);
	SRData* info = reinterpret_cast<SRData*>(handleSuite->host_lock_handle(infoH));
	if (!info) return PF_Err_BAD_CALLBACK_PARAM;

	PF_EffectWorld* input_world = nullptr;
	PF_EffectWorld* output_world = nullptr;
	ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, AF_INPUT, &input_world));
	ERR(extra->cb->checkout_output(in_data->effect_ref, &output_world));

	if (!err && input_world && output_world) {
		// 只读 PreRender 阶段快照的参数 (不再 checkout)
		dissolve::Params p;
		p.noiseScale   = 1.0f;   // 内部固定 (设计面板未暴露)
		p.scaleX       = 1.0f;
		p.scaleY       = 1.0f;
		p.brightness   = 0.5f;
		p.contrast     = 1.0f;
		p.evolution    = 0.0f;
		p.complexityL  = 4;
		p.alphaThreshold = 0.5f;
		p.quality      = info->quality;
		p.speedMapInfluenceF = info->speedInfluence;
		{
			int sm = info->speedMapMode;  // 复用字段存面板索引 (见 PreRender)
			p.speedMapMode = (sm == 5) ? 2 : 1;
			if (sm == 0) { p.speedMapMode = 0; p.speedMapInfluenceF = 0.f; }
		}
		p.speedMapChannel = info->speedMapChannel;
		p.speedMapScale   = 1.0f;
		p.blurRadius      = info->blurRadius;
		p.borderExpand    = (int)info->borderExpand;
		p.borderInfluenceF   = info->borderStrength;
		p.gammaF       = 1.0f;
		p.exposureF    = info->exposure;
		p.timeF        = 1.0f;
		p.cullB        = 0;
		p.growthSource = info->growthSource;  // 0=点 1=噪波 2=图层 (PreRender 已做 1-based 换算)
		p.blendMode    = info->blendMode;
		p.sourceMode   = info->sourceMode;
		BrushUI brush;
		brush.enable = true;
		brush.radius = info->radius;
		brush.num    = info->pointCount;
		for (int k = 0; k < 10; k++) brush.px[k] = info->px[k];
		for (int k = 0; k < 5; k++) brush.delay[k] = info->delay[k];
		brush.speed  = info->speed;
		PresetOverride po;
		po.duration  = info->durOverride;
		po.repeat    = info->repeatOverride;
		po.composite = info->compositeOverride;
		po.loopFade  = info->loopFade;
		err = renderWorldSafe(in_data, out_data, input_world, output_world,
		                      p, info->presetIdx, info->renderer, brush, po, info->viewMode,
		                      in_data->current_time, in_data->time_scale);
	}
	ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, AF_INPUT));
	handleSuite->host_unlock_handle(infoH);
	return err;
}

extern "C" DllExport
PF_Err PluginDataEntryFunction2(
	PF_PluginDataPtr inPtr,
	PF_PluginDataCB2 inPluginDataCallBackPtr,
	SPBasicSuite* inSPBasicSuitePtr,
	const char* inHostName,
	const char* inHostVersion)
{
	PF_Err result = PF_Err_INVALID_CALLBACK;
	result = PF_REGISTER_EFFECT_EXT2(
		inPtr, inPluginDataCallBackPtr,
		NAME, "ADBE TestFill2", "测试",
		AE_RESERVED_INFO, "EffectMain",
		"https://ae-plugins.docsforadobe.dev/");
	return result;
}

extern "C" DllExport
PF_Err
EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data,
	PF_ParamDef* params[], PF_LayerDef* output, void* extra)
{
	// SEH 防御: 任何命令内异常返回错误, 不拖垮 AE 进程
	PF_Err err = PF_Err_NONE;
	// 降频记录命令 (RENDER 每帧调用, 每次写盘会拖慢渲染)
	logFrame("EffectMain cmd=%d", (int)cmd);
	__try {
		switch (cmd) {
		case PF_Cmd_ABOUT: err = About(in_data, out_data, params, output); break;
		case PF_Cmd_GLOBAL_SETUP:
			glr::setLogFn(glLogSink);  // GPU 初始化/失败原因写入插件日志
			err = GlobalSetup(in_data, out_data, params, output);
			logMsg("  GLOBAL_SETUP out_flags=0x%X out_flags2=0x%X",
				(unsigned)out_data->out_flags, (unsigned)out_data->out_flags2);
			break;
		case PF_Cmd_PARAMS_SETUP: err = ParamsSetup(in_data, out_data, params, output); break;
		case PF_Cmd_RENDER:
			err = Render(in_data, out_data, params, output);
			break;
		case PF_Cmd_SMART_PRE_RENDER:
			err = PreRender(in_data, out_data, reinterpret_cast<PF_PreRenderExtra*>(extra));
			break;
		case PF_Cmd_SMART_RENDER:
			err = SmartRender(in_data, out_data, reinterpret_cast<PF_SmartRenderExtra*>(extra));
			break;
		case PF_Cmd_USER_CHANGED_PARAM: {
			// 参数变更诊断: 记录 UI 提交 (排查"切换预设无效")
			PF_ParamDef* pd = params ? params[0] : nullptr;
			A_long which = reinterpret_cast<PF_UserChangedParamExtra*>(extra)->param_index;
			if (params && which >= 0 && which < NUM_PARAMS && params[which]) {
				pd = params[which];
				logMsg("  USER_CHANGED param=%ld id=%ld type=%d value(sd)=%ld value(pd)=%ld",
				       which, (A_long)pd->uu.id, (int)pd->param_type,
				       (A_long)pd->u.sd.value, (A_long)pd->u.pd.value);
			}
			break;
		}
		default: break;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		err = PF_Err_INTERNAL_STRUCT_DAMAGED;
		logMsg("  !! SEH EXCEPTION in cmd=%d", (int)cmd);
	}
	if (err != PF_Err_NONE) logMsg("  cmd=%d returned err=0x%X", (int)cmd, (unsigned)err);
	return err;
}



