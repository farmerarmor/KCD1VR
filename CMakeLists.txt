cmake_minimum_required(VERSION 3.24)

project(KCD1VR VERSION 0.1.67 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_ALL_EXTENSIONS OFF CACHE BOOL "" FORCE)
set(BUILD_SPECIFICATION OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(INSTALL_SYMBOLS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    openxr
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK-Source.git
    GIT_TAG c07ad64839653712190e05dbd8cf460e1d239513
    GIT_SHALLOW TRUE
    GIT_SUBMODULES ""
)

FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537
    GIT_SHALLOW TRUE
    GIT_SUBMODULES ""
)

FetchContent_MakeAvailable(openxr minhook)

add_library(KCD1VR SHARED
    src/Config.cpp
    src/Config.h
    src/DInputProxy.cpp
    src/DxbcChecksum.cpp
    src/DxbcChecksum.h
    src/DlssUpscaler.cpp
    src/DlssUpscaler.h
    src/Hooks.cpp
    src/Hooks.h
    src/KcdCamera.cpp
    src/KcdCamera.h
    src/Log.cpp
    src/Log.h
    src/OpenXRRuntime.cpp
    src/OpenXRRuntime.h
    src/ShaderCapture.cpp
    src/ShaderCapture.h
    src/dinput8.def
)

add_executable(KCD1VRResolution
    src/ResolutionTool.cpp
)

target_compile_definitions(KCD1VR PRIVATE
    DIRECTINPUT_VERSION=0x0800
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    XR_USE_PLATFORM_WIN32
    XR_USE_GRAPHICS_API_D3D11
)

if(MSVC)
    target_compile_options(KCD1VR PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

target_link_libraries(KCD1VR PRIVATE
    minhook
    openxr_loader
    d3d11
    dxgi
    d3dcompiler
    "${CMAKE_CURRENT_SOURCE_DIR}/.deps/NVIDIA-DLSS/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib"
)

target_include_directories(KCD1VR PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/.deps/NVIDIA-DLSS/include"
)

target_link_libraries(KCD1VRResolution PRIVATE
    openxr_loader
)

set_target_properties(KCD1VR PROPERTIES
    OUTPUT_NAME dinput8
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist"
)

set_target_properties(KCD1VRResolution PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist"
)

add_custom_command(TARGET KCD1VR POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/KCD1VR.ini"
        "$<TARGET_FILE_DIR:KCD1VR>/KCD1VR.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/.deps/NVIDIA-DLSS/lib/Windows_x86_64/rel/nvngx_dlss.dll"
        "$<TARGET_FILE_DIR:KCD1VR>/nvngx_dlss.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/.deps/NVIDIA-DLSS/LICENSE.txt"
        "$<TARGET_FILE_DIR:KCD1VR>/NVIDIA-DLSS-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${openxr_SOURCE_DIR}/LICENSE"
        "$<TARGET_FILE_DIR:KCD1VR>/OpenXR-SDK-Source-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${minhook_SOURCE_DIR}/LICENSE.txt"
        "$<TARGET_FILE_DIR:KCD1VR>/MinHook-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
        "$<TARGET_FILE_DIR:KCD1VR>/THIRD_PARTY_NOTICES.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/KCD1VR.cfg"
        "$<TARGET_FILE_DIR:KCD1VR>/KCD1VR.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/KCD1VR-dlss.cfg"
        "$<TARGET_FILE_DIR:KCD1VR>/KCD1VR-dlss.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/KCD1VR-effects-off.cfg"
        "$<TARGET_FILE_DIR:KCD1VR>/KCD1VR-effects-off.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/KCD1VR-culling-safe.cfg"
        "$<TARGET_FILE_DIR:KCD1VR>/KCD1VR-culling-safe.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/KCD1VR-culling-off.cfg"
        "$<TARGET_FILE_DIR:KCD1VR>/KCD1VR-culling-off.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/KCD1VR-culling-balanced.cfg"
        "$<TARGET_FILE_DIR:KCD1VR>/KCD1VR-culling-balanced.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/KCD1VR-coverage-buffer-off.cfg"
        "$<TARGET_FILE_DIR:KCD1VR>/KCD1VR-coverage-buffer-off.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/KCD1VR-frustum-wide.cfg"
        "$<TARGET_FILE_DIR:KCD1VR>/KCD1VR-frustum-wide.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/KCD1VR-secondary-culling-off.cfg"
        "$<TARGET_FILE_DIR:KCD1VR>/KCD1VR-secondary-culling-off.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/KCD1VR-distant-culling-safe.cfg"
        "$<TARGET_FILE_DIR:KCD1VR>/KCD1VR-distant-culling-safe.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/KCD1VR-check-occlusion-off.cfg"
        "$<TARGET_FILE_DIR:KCD1VR>/KCD1VR-check-occlusion-off.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/KCD1VR-object-occlusion-off.cfg"
        "$<TARGET_FILE_DIR:KCD1VR>/KCD1VR-object-occlusion-off.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/KCD1VR-occlusion-worker-off.cfg"
        "$<TARGET_FILE_DIR:KCD1VR>/KCD1VR-occlusion-worker-off.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/KCD1VR-occlusion-margin-low.cfg"
        "$<TARGET_FILE_DIR:KCD1VR>/KCD1VR-occlusion-margin-low.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/KCD1VR-occlusion-margin-medium.cfg"
        "$<TARGET_FILE_DIR:KCD1VR>/KCD1VR-occlusion-margin-medium.cfg"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/KCD1VR-occlusion-margin-high.cfg"
        "$<TARGET_FILE_DIR:KCD1VR>/KCD1VR-occlusion-margin-high.cfg"
)
