@echo off
setlocal
set "VCVARS="
if exist "D:\Software\MS\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=D:\Software\MS\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
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

cl /nologo /std:c++17 /EHsc /O2 /utf-8 /DUNICODE /D_UNICODE ^
   /I. /I..\..\third_party\webview2\build\native\include ^
   d2d_app.cpp stages.cpp auth.cpp icons.cpp ui_main.cpp user_state.cpp ^
   chat.cpp modals.cpp persist.cpp toast.cpp i18n.cpp fetch.cpp ws_user.cpp ^
   tray.cpp sticker.cpp webview.cpp main.cpp /link ^
   /SUBSYSTEM:WINDOWS /OUT:LauncherD2D.exe
set RC=%ERRORLEVEL%
del /q *.obj 2>nul
if "%RC%"=="0" (
    if not exist ..\..\dist mkdir ..\..\dist
    copy /y LauncherD2D.exe ..\..\dist\LauncherD2D.exe >nul
    if exist ..\..\assets\images\games\cs2_header.jpg (
        copy /y ..\..\assets\images\games\cs2_header.jpg ..\..\dist\cs2_header.jpg >nul
    )
    if exist ..\..\third_party\webview2\runtimes\win-x64\native\WebView2Loader.dll (
        copy /y ..\..\third_party\webview2\runtimes\win-x64\native\WebView2Loader.dll ..\..\dist\WebView2Loader.dll >nul
    )
    echo [build_d2d] LauncherD2D.exe + cs2_header.jpg + WebView2Loader.dll -^> dist\
)
popd
exit /b %RC%
