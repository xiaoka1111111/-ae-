#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
	#include "AE_General.r"
#endif
resource 'PiPL' (16000) {
	{	/* array properties */
		Kind {
			AEEffect
		},
		Name {
			"TestFill"
		},
		Category {
			"Test"
		},
#ifdef AE_OS_WIN
    #if defined(AE_PROC_INTELx64)
		CodeWin64X86 {"EffectMain"},
    #elif defined(AE_PROC_ARM64)
		CodeWinARM64 {"EffectMain"},
    #endif
#elif defined(AE_OS_MAC)
		CodeMacIntel64 {"EffectMain"},
		CodeMacARM64 {"EffectMain"},
#endif
		AE_PiPL_Version {
			2,
			0
		},
		AE_Effect_Spec_Version {
			PF_PLUG_IN_VERSION,
			PF_PLUG_IN_SUBVERS
		},
		AE_Effect_Version {	
			1081345	/* must match code PF_VERSION */
		},
		AE_Effect_Info_Flags {
			0
		},
		AE_Effect_Global_OutFlags {
			0x2000404	/* PIX_INDEPENDENT | DEEP_COLOR_AWARE | NON_PARAM_VARY
			                  (输出依赖帧时间 — AE 必须逐帧重渲, 否则跨时间复用首帧:
			                  用户实测"播放无动画, 拖图层才刷新") */
		},
		AE_Effect_Global_OutFlags_2 {
			0x0	/* 传统 Render 路径: 无 SMART_RENDER / FLOAT_COLOR_AWARE */
		},
		AE_Effect_Match_Name {
			"ADBE TestFill2"
		},
		AE_Reserved_Info {
			0
		},
		AE_Effect_Support_URL {
			"https://ae-plugins.docsforadobe.dev/"
		}
	}
};
