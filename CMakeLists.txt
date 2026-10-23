cmake_minimum_required(VERSION 3.24)
project(Persona3ReloadDLSS VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE)
add_compile_options(/W4 /permissive- /utf-8)

# NVIDIA's DLSS SDK is downloaded by tools/fetch_dlss_sdk.ps1 and stays under NVIDIA's license.
set(DLSS_SDK_DIR ${CMAKE_SOURCE_DIR}/third_party/dlss CACHE PATH "NVIDIA DLSS SDK directory")
if(NOT EXISTS ${DLSS_SDK_DIR}/lib/nvsdk_ngx_s.lib)
  message(FATAL_ERROR "DLSS SDK not found in ${DLSS_SDK_DIR}. Run tools/fetch_dlss_sdk.ps1 first.")
endif()

# ReShade's state tracking helper (BSD-3-Clause OR MIT).
add_library(reshade_utils STATIC third_party/reshade/state_tracking.cpp)
target_include_directories(reshade_utils PUBLIC third_party/reshade)
target_compile_options(reshade_utils PRIVATE /W0)

# Shaders are compiled to DXBC headers with the Windows SDK's fxc: shader(<symbol> <file> <profile> <entry> [defines])
# writes ${SHADER_DIR}/<symbol>.h with the bytecode array <symbol>.
find_program(FXC fxc REQUIRED)
set(SHADER_DIR ${CMAKE_BINARY_DIR}/shaders)
set(SHADER_HEADERS)
function(shader symbol file profile entry)
  set(defines)
  foreach(define ${ARGN})
    list(APPEND defines /D ${define})
  endforeach()
  set(header ${SHADER_DIR}/${symbol}.h)
  add_custom_command(
    OUTPUT ${header}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
    COMMAND ${FXC} /nologo /T ${profile} /E ${entry} /O3 ${defines} /Vn ${symbol} /Fh ${header}
            ${CMAKE_SOURCE_DIR}/src/${file}
    DEPENDS src/${file}
    VERBATIM)
  set(SHADER_HEADERS ${SHADER_HEADERS} ${header} PARENT_SCOPE)
endfunction()

shader(kMotionVectorsCs motion_vectors.hlsl cs_5_0 main)

configure_file(src/version.rc.in ${CMAKE_BINARY_DIR}/version.rc @ONLY)

add_library(Persona3ReloadDLSS SHARED src/addon.cpp src/console_variables.cpp src/descriptors.cpp src/dlss.cpp
            ${CMAKE_BINARY_DIR}/version.rc ${SHADER_HEADERS})
target_include_directories(Persona3ReloadDLSS PRIVATE src third_party/imgui ${SHADER_DIR})
target_include_directories(Persona3ReloadDLSS SYSTEM PRIVATE ${DLSS_SDK_DIR}/include)
target_link_libraries(Persona3ReloadDLSS PRIVATE reshade_utils ${DLSS_SDK_DIR}/lib/nvsdk_ngx_s.lib d3d12 dxgi)
set_target_properties(Persona3ReloadDLSS PROPERTIES SUFFIX ".addon64")
