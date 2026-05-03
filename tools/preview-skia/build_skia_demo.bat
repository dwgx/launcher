@echo off
setlocal
call "D:\Software\MS\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo vcvars failed & exit /b 1)
pushd "%~dp0"

set SKIA=..\..\third_party\skia
set SKIA_LIB=%SKIA%\out\Release-x64

cl /nologo /std:c++17 /EHsc /O2 /utf-8 /DUNICODE /D_UNICODE ^
   /I"%SKIA%" ^
   skia_demo.cpp /link ^
   "%SKIA_LIB%\skia.lib" ^
   "%SKIA_LIB%\skshaper.lib" ^
   "%SKIA_LIB%\skunicode.lib" ^
   "%SKIA_LIB%\harfbuzz.lib" ^
   "%SKIA_LIB%\icu.lib" ^
   "%SKIA_LIB%\skcms.lib" ^
   "%SKIA_LIB%\freetype2.lib" ^
   "%SKIA_LIB%\libpng.lib" ^
   "%SKIA_LIB%\libjpeg.lib" ^
   "%SKIA_LIB%\libwebp.lib" ^
   "%SKIA_LIB%\libwebp_sse41.lib" ^
   "%SKIA_LIB%\zlib.lib" ^
   "%SKIA_LIB%\expat.lib" ^
   "%SKIA_LIB%\bentleyottmann.lib" ^
   "%SKIA_LIB%\wuffs.lib" ^
   /SUBSYSTEM:WINDOWS /OUT:LauncherSkiaPoC.exe
set RC=%ERRORLEVEL%
del /q skia_demo.obj 2>nul
popd
exit /b %RC%
