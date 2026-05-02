# Clay 单头文件布局库：https://github.com/nicbarker/clay
# 直接放 third_party/clay/clay.h，CMake 只暴露包含路径。

set(CLAY_ROOT "${CMAKE_SOURCE_DIR}/third_party/clay" CACHE PATH "Path to clay.h")

if(NOT EXISTS "${CLAY_ROOT}/clay.h")
    message(WARNING "clay.h not found at ${CLAY_ROOT}. Run scripts/fetch-clay.ps1 first.")
endif()

add_library(clay INTERFACE)
add_library(clay::clay ALIAS clay)

target_include_directories(clay INTERFACE "${CLAY_ROOT}")

# Why: Clay 默认 Arena 8MB，卡片网格大时不够，提到 32MB
target_compile_definitions(clay INTERFACE
    CLAY_MAX_ELEMENT_COUNT=8192
)
