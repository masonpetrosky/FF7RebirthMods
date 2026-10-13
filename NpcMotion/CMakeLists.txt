cmake_minimum_required(VERSION 3.24)
project(FF7RebirthNpcMotion VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE)
add_compile_options(/W4 /permissive- /utf-8)

# The ReShade add-on.
add_library(FF7RebirthNpcMotion SHARED src/addon.cpp src/descriptors.cpp src/dlss_probe.cpp src/shader_reflection.cpp)
target_include_directories(FF7RebirthNpcMotion PRIVATE src third_party/reshade third_party/imgui)
target_link_libraries(FF7RebirthNpcMotion PRIVATE dxguid)
set_target_properties(FF7RebirthNpcMotion PROPERTIES SUFFIX ".addon64")

# Offline check of the bone register lookup against shaders saved from a capture (tools/rdc_shaderdump.py).
add_executable(reflect_test tests/reflect_test.cpp src/shader_reflection.cpp)
target_include_directories(reflect_test PRIVATE src)
target_link_libraries(reflect_test PRIVATE dxguid)
