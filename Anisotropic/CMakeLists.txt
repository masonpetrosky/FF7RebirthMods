cmake_minimum_required(VERSION 3.24)
project(FF7RebirthAnisotropic VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE)
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8>")

# The ReShade add-on, with a version resource for ReShade's add-on list.
configure_file(src/version.rc.in version.rc @ONLY)
add_library(FF7RebirthAnisotropic SHARED src/addon.cpp ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
target_include_directories(FF7RebirthAnisotropic PRIVATE src third_party/reshade third_party/imgui)
set_target_properties(FF7RebirthAnisotropic PROPERTIES SUFFIX ".addon64")
