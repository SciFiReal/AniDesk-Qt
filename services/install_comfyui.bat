@echo off
rem ============================================================
rem  ComfyUI + LivePortrait 一键安装脚本（动图服务后端）
rem  用法：把本文件放到你希望安装的盘符根目录/目标目录后运行，
rem        或直接双击。全程需要联网，总下载约 4~6GB。
rem
rem  硬件要求：NVIDIA GPU（4GB 显存可跑，建议先关闭桌宠/Ollama 腾出显存）
rem  依赖：已安装 Git（用于克隆自定义节点）
rem ============================================================
setlocal
rem ==== 按需修改安装目录 ====
set ROOT=%~dp0ComfyUI-AniDesk
set PYTHON=%ROOT%\python_embeded\python.exe

echo [1/5] 下载 ComfyUI Windows 便携版（约 2.5GB，请耐心等待）...
mkdir "%ROOT%" 2>nul
curl -L -o "%ROOT%\comfyui.7z" https://github.com/comfyanonymous/ComfyUI/releases/latest/download/ComfyUI_windows_portable_nvidia.7z
if errorlevel 1 ( echo 下载失败，请手动从 https://github.com/comfyanonymous/ComfyUI/releases 下载 & pause & exit /b 1 )

echo [2/5] 解压（需要系统已安装 7-Zip，或用 WinRAR）...
7z x "%ROOT%\comfyui.7z" -o"%ROOT%" -y
if errorlevel 1 ( echo 解压失败，请手动解压 comfyui.7z 到 %ROOT% & pause & exit /b 1 )

rem 定位 ComfyUI 目录（便携版解压后通常为 ComfyUI_windows_portable）
set PORTABLE=%ROOT%\ComfyUI_windows_portable
if not exist "%PORTABLE%" set PORTABLE=%ROOT%

echo [3/5] 克隆 LivePortrait 自定义节点...
"%PORTABLE%\python_embeded\python.exe" -m pip install GitPython
git clone https://github.com/dfl/ComfyUI-AdvancedLivePortrait "%PORTABLE%\ComfyUI\custom_nodes\ComfyUI-AdvancedLivePortrait"
if exist "%PORTABLE%\ComfyUI\custom_nodes\ComfyUI-AdvancedLivePortrait\requirements.txt" (
    "%PORTABLE%\python_embeded\python.exe" -m pip install -r "%PORTABLE%\ComfyUI\custom_nodes\ComfyUI-AdvancedLivePortrait\requirements.txt"
)

echo [4/5] 下载 LivePortrait 模型与示例驱动视频...
rem 该节点首次运行会自动下载模型；示例驱动视频可从 LivePortrait 仓库获取：
rem https://github.com/KwaiVGI/LivePortrait/tree/main/assets/examples/driving
rem 下载后按 blink.mp4 / speak.mp4 / smile.mp4 命名，放到：
echo     %~dp0driving\
mkdir "%~dp0driving" 2>nul

echo [5/5] 完成！后续步骤：
echo   1. 运行 %PORTABLE%\run_nvidia_gpu.bat 启动 ComfyUI (端口 8188)
echo   2. pip install fastapi uvicorn httpx 后运行:
echo      python %~dp0comfyui_animate_server.py
echo   3. 在桌宠里拖入图片，动图生成将走 ComfyUI 管线
pause
