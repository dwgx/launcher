@echo off
setlocal
call "D:\Software\MS\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo vcvars failed & exit /b 1)
pushd "%~dp0"
cl /nologo /std:c++17 /EHsc /O2 /utf-8 /DUNICODE /D_UNICODE loading_demo.cpp /link ^
   gdiplus.lib user32.lib gdi32.lib dwmapi.lib /SUBSYSTEM:WINDOWS /OUT:LauncherPreview.exe
set RC=%ERRORLEVEL%
del /q loading_demo.obj 2>nul
popd
exit /b %RC%
