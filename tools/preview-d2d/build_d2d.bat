@echo off
setlocal
call "D:\Software\MS\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo vcvars failed & exit /b 1)
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
