@echo off
setlocal
rem ============================================================================
rem build_d2d_visual.bat — build_d2d.bat 的克隆 + visual_smoke.cpp + /DLAUNCHER_VISUAL_SMOKE
rem 用途：编出带「可插拔视觉冒烟钩子」的 LauncherD2D.exe（详见 visual_smoke.h /
rem       tmp/visual-smoke/PLAN-*.md）。正常 build_d2d.bat 保持纯净、宏未定义、钩子编译为空。
rem 运行冒烟：先 build_d2d_visual.bat，再 set LAUNCHER_VISUAL_SMOKE=1 后启动 exe
rem       （或加 --visual-smoke），产物 PNG 落在 tools/preview-d2d/.visual-smoke/
rem       或 env LAUNCHER_VISUAL_SMOKE_OUT 指定目录。
rem 移除：删本文件 + visual_smoke.{h,cpp} + main.cpp 里 #ifdef 块 + 上面的 #include。
rem ============================================================================
set "VCVARS="
if exist "D:\Software\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=D:\Software\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    for /f "usebackq tokens=*" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%I\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat"
    )
)
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS (
    echo vcvars64.bat not found
    exit /b 1
)
call "%VCVARS%" >nul
if errorlevel 1 (echo vcvars failed: "%VCVARS%" & exit /b 1)
pushd "%~dp0"

rem 生产 host 通过环境变量注入，不写死进源码（脱敏）。
set "HOST_DEF="
if defined LAUNCHER_DEFAULT_HOST set "HOST_DEF=/DLAUNCHER_DEFAULT_HOST=L\"%LAUNCHER_DEFAULT_HOST%\""
if /I "%LAUNCHER_DEFAULT_SCHEME%"=="http" set "HOST_DEF=%HOST_DEF% /DLAUNCHER_DEFAULT_SECURE=0"
if defined LAUNCHER_DEFAULT_PORT set "HOST_DEF=%HOST_DEF% /DLAUNCHER_DEFAULT_PORT=%LAUNCHER_DEFAULT_PORT%"

cl /nologo /std:c++17 /EHsc /O2 /utf-8 /DUNICODE /D_UNICODE /DLAUNCHER_VISUAL_SMOKE %HOST_DEF% ^
   /I. /I..\..\third_party\webview2\build\native\include ^
   d2d_app.cpp stages.cpp auth.cpp icons.cpp ui_main.cpp user_state.cpp ^
   chat.cpp chat_state.cpp chat_announcements.cpp chat_identity.cpp chat_net.cpp chat_paint.cpp modals.cpp persist.cpp toast.cpp i18n.cpp fetch.cpp ws_user.cpp ^
   tray.cpp sticker.cpp webview.cpp render\decode_worker.cpp download_pool.cpp visual_smoke.cpp main.cpp /link ^
   /SUBSYSTEM:WINDOWS /OUT:LauncherD2D.exe
set RC=%ERRORLEVEL%
del /q *.obj 2>nul
if "%RC%"=="0" (
    if not exist ..\..\dist mkdir ..\..\dist
    copy /y LauncherD2D.exe ..\..\dist\LauncherD2D.exe >nul
    if exist ..\..\assets\images\games\cs2_header.jpg (
        copy /y ..\..\assets\images\games\cs2_header.jpg ..\..\dist\cs2_header.jpg >nul
    )
    if exist ..\..\assets\images\games\cs2_header.mp4 (
        copy /y ..\..\assets\images\games\cs2_header.mp4 ..\..\dist\cs2_header.mp4 >nul
    )
    if exist ..\..\third_party\webview2\runtimes\win-x64\native\WebView2Loader.dll (
        copy /y ..\..\third_party\webview2\runtimes\win-x64\native\WebView2Loader.dll ..\..\dist\WebView2Loader.dll >nul
    )
    echo [build_d2d_visual] LauncherD2D.exe ^(+visual-smoke hook^) -^> dist\
)
popd
exit /b %RC%
