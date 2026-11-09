# ngx_probe.exe: which Vulkan instance / device extensions NGX DLSS (SR) and DLSS-G (FG) need on this PC's
# GPU and driver, and whether each feature is supported (plan-dlss.md D0). Built like gpu/dlss_bridge:
#   cmake -S research/tools/ngx_probe -B out/ngx-probe -G "Visual Studio 17 2022" -A x64 \
#         -DDLSS_SDK_ROOT=<DLSS SDK> -DVULKAN_INCLUDE=<out/vkinclude>
#   cmake --build out/ngx-probe --config Release
cmake_minimum_required(VERSION 3.20)
project(ngx_probe LANGUAGES CXX)
set(DLSS_SDK_ROOT "" CACHE PATH "NVIDIA DLSS SDK root")
set(VULKAN_INCLUDE "" CACHE PATH "Vulkan-Headers include directory")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
add_executable(ngx_probe ngx_probe.cpp)
target_include_directories(ngx_probe PRIVATE "${DLSS_SDK_ROOT}/include" "${VULKAN_INCLUDE}")
target_link_libraries(ngx_probe PRIVATE "${DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
target_compile_definitions(ngx_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN VK_NO_PROTOTYPES)
