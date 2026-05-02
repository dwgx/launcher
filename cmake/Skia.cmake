# Skia 集成：使用 aseprite 预编译版本
# https://github.com/aseprite/skia/releases
#
# Why aseprite/skia: 官方 Skia 用 GN，新手两天起步；aseprite 提供 Win/x64 静态库 + 头文件，开箱即用。
# Why not vcpkg skia: vcpkg 的 skia port 不稳定，且默认配置不带 GL backend。
#
# 用法：把解压后的 Skia 放到 third_party/skia/，包含 include/ 和 out/Release-x64/skia.lib
#
# 第一次构建前请运行 scripts/fetch-skia.ps1 拉取并解压。

set(SKIA_ROOT "${CMAKE_SOURCE_DIR}/third_party/skia" CACHE PATH "Path to aseprite/skia release")

if(NOT EXISTS "${SKIA_ROOT}/include/core/SkCanvas.h")
    message(WARNING "Skia not found at ${SKIA_ROOT}. Run scripts/fetch-skia.ps1 first. "
                    "Build will fail until Skia headers are present.")
endif()

add_library(skia STATIC IMPORTED GLOBAL)
add_library(skia::skia ALIAS skia)

set_target_properties(skia PROPERTIES
    IMPORTED_LOCATION_RELEASE "${SKIA_ROOT}/out/Release-x64/skia.lib"
    IMPORTED_LOCATION_DEBUG   "${SKIA_ROOT}/out/Debug-x64/skia.lib"
    IMPORTED_CONFIGURATIONS "Release;Debug"
    INTERFACE_INCLUDE_DIRECTORIES "${SKIA_ROOT};${SKIA_ROOT}/include"
    INTERFACE_COMPILE_DEFINITIONS "SK_GL=1;SK_R32_SHIFT=16;SK_GANESH"
)

target_link_libraries(skia INTERFACE
    opengl32
    user32
    gdi32
)
