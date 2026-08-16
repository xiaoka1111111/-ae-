@echo off
rem ============================================================
rem  一键推送本仓库到 GitHub
rem  前置: 已在 https://github.com/new 创建空仓库「关于ae插件填充算法研究」
rem        并生成 PAT (GitHub → Settings → Developer settings →
rem        Personal access tokens → Tokens (classic) → Generate →
rem        勾选 repo → 复制)
rem ============================================================
setlocal
cd /d %~dp0

git config http.proxy http://127.0.0.1:7897
git config https.proxy http://127.0.0.1:7897

set /p TOKEN=Paste your GitHub PAT (只粘贴令牌, 不显示明文请回车后粘贴): 
if "%TOKEN%"=="" (
    echo [ERROR] 未输入 PAT
    exit /b 1
)

git remote remove origin 2>nul
git remote add origin "https://%TOKEN%@github.com/xiaoka1111111/关于ae插件填充算法研究.git"
git push -u origin master
if errorlevel 1 (
    echo.
    echo [FAILED] 推送失败。常见原因:
    echo   - 仓库还没创建 (先去 https://github.com/new 创建空仓库「关于ae插件填充算法研究」)
    echo   - PAT 权限不足 (需要勾选 repo)
    echo   - 代理未运行 (127.0.0.1:7897)
    exit /b 1
)
echo.
echo [OK] 已推送到 https://github.com/xiaoka1111111/关于ae插件填充算法研究
echo 安全提醒: PAT 已写入本仓库 remote 配置, 建议推送完成后执行:
echo   git remote set-url origin https://github.com/xiaoka1111111/关于ae插件填充算法研究.git
pause
