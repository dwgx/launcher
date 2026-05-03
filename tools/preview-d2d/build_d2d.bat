@echo off
setlocal
call "D:\Software\MS\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo vcvars failed & exit /b 1)
pushd "%~dp0"

cl /nologo /std:c++17 /EHsc /O2 /utf-8 /DUNICODE /D_UNICODE /I. ^
   d2d_app.cpp stages.cpp auth.cpp icons.cpp ui_main.cpp user_state.cpp ^
   chat.cpp modals.cpp persist.cpp toast.cpp i18n.cpp fetch.cpp ws_user.cpp ^
   tray.cpp sticker.cpp main.cpp /link ^
   /SUBSYSTEM:WINDOWS /OUT:LauncherD2D.exe
set RC=%ERRORLEVEL%
del /q *.obj 2>nul
if "%RC%"=="0" (
    if not exist ..\..\dist mkdir ..\..\dist
    copy /y LauncherD2D.exe ..\..\dist\LauncherD2D.exe >nul
    echo [build_d2d] LauncherD2D.exe -^> dist\
)
popd
exit /b %RC%
