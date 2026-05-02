cmake_minimum_required(VERSION 3.24)

set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
    CACHE STRING "vcpkg toolchain")

project(Launcher
    VERSION 0.1.0
    DESCRIPTION "Windows desktop game launcher"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "Launcher targets Windows 11 x64 only.")
endif()

# VMProtect-friendly: no RTTI, no exceptions, hidden symbols, /Zc:__cplusplus
if(MSVC)
    add_compile_options(
        /W4
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        /utf-8
        /MP
        /GR-
        /EHs-c-
    )
    add_compile_definitions(
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
        _CRT_SECURE_NO_WARNINGS
    )
    # Release: enable LTCG + remove default debug info from binary; PDB still emitted separately
    add_link_options($<$<CONFIG:Release>:/LTCG>)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

include(cmake/Skia.cmake)
include(cmake/Clay.cmake)

find_package(glfw3 CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(harfbuzz CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(launcher WIN32
    src/app/main.cpp
    src/app/window.cpp
    src/app/event_loop.cpp

    src/ui/render/skia_renderer.cpp
    src/ui/render/font_manager.cpp
    src/ui/theme/theme_manager.cpp

    # Anim 框架 — 全 header-only 或单 .cpp
    src/ui/anim/animator.cpp

    # I18N
    src/ui/i18n/locale.cpp

    # Components / Views — Phase 1 仅含 spinner + loading；其他骨架在 Phase 2 接 .cpp 时启用
    src/ui/components/spinner.cpp
    src/ui/components/loading_card.cpp
    src/ui/views/loading_view.cpp

    src/core/event_bus/event_bus.cpp

    src/storage/registry.cpp
    src/storage/sqlite_store.cpp

    src/crypto/crypt_str.cpp
    src/crypto/dpapi_seal.cpp

    src/native/hwid.cpp
    src/native/dyn_api.cpp

    src/net/http_client.cpp
)

target_include_directories(launcher PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(launcher PRIVATE
    skia::skia
    clay::clay
    glfw
    fmt::fmt
    spdlog::spdlog
    unofficial-sodium::sodium
    CURL::libcurl
    unofficial::sqlite3::sqlite3
    protobuf::libprotobuf
    harfbuzz::harfbuzz
    nlohmann_json::nlohmann_json

    # Win32
    user32
    gdi32
    shell32
    advapi32
    crypt32
    dwmapi
    iphlpapi
    setupapi
    wbemuuid
    opengl32
    ws2_32
    tbs              # TPM Base Services for HWID
)

set_target_properties(launcher PROPERTIES
    OUTPUT_NAME "Launcher"
    DEBUG_POSTFIX "_d"
)

# Copy assets next to the built binary so dev runs find fonts/images
add_custom_command(TARGET launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:launcher>/assets"
)

option(LAUNCHER_BUILD_TESTS "Build tests" OFF)
if(LAUNCHER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
