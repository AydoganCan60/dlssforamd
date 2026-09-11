# Copyright (c) 2026 AydoganCan60- MIT License
cmake_minimum_required(VERSION 3.21)
project(dlssforamd VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This project targets Windows. Configure with x86_64-w64-mingw32-cmake or a MinGW toolchain file.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(DETOURS_ROOT "${CMAKE_SOURCE_DIR}/external/Detours" CACHE PATH "Microsoft Detours 4.0.1 source directory")
set(FIDELITYFX_SDK_DIR "${CMAKE_SOURCE_DIR}/external/FidelityFX-SDK" CACHE PATH "AMD FidelityFX SDK source directory")
set(NGX_SDK_DIR "" CACHE PATH "NVIDIA NGX SDK directory containing include/nvsdk_ngx.h")

foreach(required_file
        "${DETOURS_ROOT}/src/detours.h"
        "${FIDELITYFX_SDK_DIR}/Kits/FidelityFX/api/include/ffx_api.h"
        "${FIDELITYFX_SDK_DIR}/Kits/FidelityFX/upscalers/include/ffx_upscale.h"
        "${NGX_SDK_DIR}/include/nvsdk_ngx.h")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Missing dependency file: ${required_file}. See README.md.")
    endif()
endforeach()

add_library(detours STATIC
    "${DETOURS_ROOT}/src/detours.cpp"
    "${DETOURS_ROOT}/src/disasm.cpp"
    "${DETOURS_ROOT}/src/image.cpp"
    "${DETOURS_ROOT}/src/modules.cpp"
)
target_include_directories(detours PUBLIC "${DETOURS_ROOT}/src")
target_compile_definitions(detours PRIVATE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)

add_library(version SHARED src/main.cpp src/version_proxy.cpp)
set_target_properties(version PROPERTIES PREFIX "" OUTPUT_NAME "version")
target_include_directories(version PRIVATE
    "${NGX_SDK_DIR}/include"
    "${FIDELITYFX_SDK_DIR}/Kits/FidelityFX/api/include"
    "${FIDELITYFX_SDK_DIR}/Kits/FidelityFX/upscalers/include"
)
target_compile_definitions(version PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
target_compile_options(version PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(version PRIVATE detours d3d12 dxgi)

install(TARGETS version RUNTIME DESTINATION . LIBRARY DESTINATION .)
