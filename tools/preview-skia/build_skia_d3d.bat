@echo off
setlocal
call "D:\Software\MS\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo vcvars failed & exit /b 1)
pushd "%~dp0"

cl /nologo /std:c++17 /EHsc /O2 /utf-8 /DUNICODE /D_UNICODE ^
   skia_d3d.cpp /link /SUBSYSTEM:WINDOWS /OUT:LauncherSkiaD3D.exe
set RC=%ERRORLEVEL%
del /q skia_d3d.obj 2>nul
popd
exit /b %RC%
