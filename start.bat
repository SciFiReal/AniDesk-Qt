@echo off
rem ============================================================
rem  AniDesk-Qt 一键启动脚本
rem  依次拉起：Ollama 服务 → 本地抠图服务(venv) → 桌宠本体
rem  桌宠自带单实例守卫，重复运行本脚本不会出现多只宠物
rem ============================================================
setlocal
set OLLAMA=C:\Users\Luo\AppData\Local\Programs\Ollama\ollama.exe
set SVCDIR=%~dp0services

rem ---- 1) Ollama 服务（聊天/人设提取需要；不启动也能跑，仅聊天不可用）----
tasklist /FI "IMAGENAME eq ollama.exe" 2>nul | find /I "ollama.exe" >nul
if errorlevel 1 (
    echo [AniDesk] Starting Ollama...
    start "Ollama" /MIN "%OLLAMA%" serve
    timeout /t 3 /nobreak >nul
)

rem ---- 2) 本地抠图服务（透明角色需要；首次抠图自动下载 U2Net 模型）----
netstat -ano 2>nul | find /I "127.0.0.1:8765" | find /I "LISTENING" >nul
if errorlevel 1 (
    if exist "%SVCDIR%\.venv\Scripts\python.exe" (
        echo [AniDesk] Starting RMBG service...
        start "AniDesk-RMBG" /MIN "%SVCDIR%\.venv\Scripts\python.exe" "%SVCDIR%\rmbg_server.py"
        timeout /t 2 /nobreak >nul
    ) else (
        echo [AniDesk] RMBG venv not found, skip cutout service ^(see services/README.md^)
    )
)

rem ---- 3) 桌宠本体 ----
echo [AniDesk] Starting pet...
start "" "%~dp0build\AniDesk-Qt.exe"
exit /b 0
