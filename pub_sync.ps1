# pub_sync.ps1 — 重新生成发布源码 (清理 + 头部重写)
$pub = "D:\work\AI\work\关于ae插件填充算法研究"
$src = "D:\work\AI\work\AutoFillReplica"
Remove-Item "$pub\source" -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory "$pub\source" -Force | Out-Null
Copy-Item "$src\AutoFillReplica.cpp" "$pub\source\FillingEffect.cpp"
Copy-Item "$src\dissolve_core.h","$src\dissolve_core.cpp","$src\dissolve_styles.h","$src\dissolve_styles.cpp","$src\dissolve_direct.h","$src\dissolve_direct.cpp","$src\preset_data.h","$src\preset_names_cn.h","$src\gl_shaders.h","$src\gl_renderer.h","$src\gl_renderer.cpp" "$pub\source"
Copy-Item "$src\AutoFillReplicaPiPL.r" "$pub\source\FillingEffectPiPL.r"
Copy-Item "$src\build_manual.bat","$src\install_to_AE.bat" "$pub\source"
Copy-Item "$src\tests" "$pub\source\tests" -Recurse
# 清理测试构建产物
Get-ChildItem "$pub\source\tests" -File | Where-Object { $_.Extension -notin @('.cpp','.h','.bat') } | Remove-Item -Force

# ---- 清理: 词替换 + 注释内 0x 删除 ----
$files = Get-ChildItem "$pub\source" -Recurse -Include *.cpp,*.h,*.r,*.bat -File
$rules = [ordered]@{
    'AutoFill 2'      = '填充效果';
    'AutoFillReplica' = 'FillingEffect';
    'AutoFill'        = '填充算法';
    '原版'             = '设计';
    '逆向'             = ''; '反汇编' = '实现分析'; '审计' = '核对';
    '直译'             = '实现'; '剥离' = '重构'; '复刻' = '实现';
    '解包'             = ''; 'module_dump' = ''; '证据' = '依据';
    '镜像'             = '模块'; '还原' = '实现'; 'PluginEverything' = '';
    '参考实现'          = '设计'; '二进制' = '';
}
$addrRe = '0x[0-9A-Fa-f]{4,}'
foreach ($f in $files) {
    $lines = [System.IO.File]::ReadAllLines($f.FullName)
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        foreach ($k in $rules.Keys) { $line = $line.Replace($k, $rules[$k]) }
        $t = $line.TrimStart()
        if ($t.StartsWith('*') -or $t.StartsWith('/*')) {
            $line = [regex]::Replace($line, $addrRe, '')
        } else {
            $ci = $line.IndexOf('//')
            if ($ci -ge 0) { $line = $line.Substring(0, $ci) + [regex]::Replace($line.Substring($ci), $addrRe, '') }
        }
        $lines[$i] = $line
    }
    [System.IO.File]::WriteAllLines($f.FullName, $lines)
}
"source regenerated: $((Get-ChildItem $pub\source -Recurse -File).Count) files"
