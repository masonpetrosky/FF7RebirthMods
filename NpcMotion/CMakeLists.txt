cmake_minimum_required(VERSION 3.24)
project(FF7RebirthNpcMotion VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE)
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8>")

# NVIDIA's DLSS SDK (the NGX headers; the static library only for tests/rr_test.cpp) is downloaded by
# tools/fetch_dlss_sdk.ps1 and stays under NVIDIA's license.
set(DLSS_SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/dlss CACHE PATH "NVIDIA DLSS SDK directory")
if(NOT EXISTS ${DLSS_SDK_DIR}/include/nvsdk_ngx_helpers_dlssd_d3d.h)
  message(FATAL_ERROR "DLSS SDK not found in ${DLSS_SDK_DIR}. Run tools/fetch_dlss_sdk.ps1 first.")
endif()

# Ray Reconstruction's guides pass (src/rr_guides.hlsl), compiled to DXIL headers with the Windows SDK's dxc (which
# signs them with the dxil.dll next to it): shader(<symbol> <file> <profile> <entry>) writes ${SHADER_DIR}/<symbol>.h.
find_program(FXC fxc REQUIRED)
get_filename_component(SDK_BIN_DIR ${FXC} DIRECTORY)
find_program(DXC dxc HINTS ${SDK_BIN_DIR} NO_DEFAULT_PATH REQUIRED)
set(SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(SHADER_HEADERS)
function(shader symbol file profile entry)
  set(header ${SHADER_DIR}/${symbol}.h)
  add_custom_command(
    OUTPUT ${header}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
    COMMAND ${DXC} -nologo -T ${profile} -E ${entry} -O3 -HV 2021 -Vn ${symbol} -Fh ${header} ${CMAKE_CURRENT_SOURCE_DIR}/src/${file}
    DEPENDS src/${file}
    VERBATIM)
  set(SHADER_HEADERS ${SHADER_HEADERS} ${header} PARENT_SCOPE)
endfunction()
shader(kRrGuides rr_guides.hlsl cs_6_0 Guides)
shader(kRrShowGuide rr_guides.hlsl cs_6_0 ShowGuide)

# The ReShade add-on, with a version resource for ReShade's add-on list.
configure_file(src/version.rc.in version.rc @ONLY)
add_library(FF7RebirthNpcMotion SHARED src/addon.cpp src/descriptors.cpp src/dlss_probe.cpp src/ray_reconstruction.cpp
  src/shader_reflection.cpp ${SHADER_HEADERS} ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
target_include_directories(FF7RebirthNpcMotion PRIVATE src ${SHADER_DIR} ../third_party/reshade ../third_party/imgui)
target_include_directories(FF7RebirthNpcMotion SYSTEM PRIVATE ${DLSS_SDK_DIR}/include)
target_link_libraries(FF7RebirthNpcMotion PRIVATE d3d12 dxguid)

# Offline check of Ray Reconstruction's path on this PC (D3D12 debug layer, NGX initialised like the game's, the game's
# own nvngx_dlssd.dll): rr_test.exe <the game's Win64 folder> <scratch folder>.
if(EXISTS ${DLSS_SDK_DIR}/lib/nvsdk_ngx_s.lib)
  add_executable(rr_test tests/rr_test.cpp src/ray_reconstruction.cpp ${SHADER_HEADERS})
  target_compile_definitions(rr_test PRIVATE NPCMOTION_RR_WITH_NGX_LIBRARY)
  target_include_directories(rr_test PRIVATE src ${SHADER_DIR})
  target_include_directories(rr_test SYSTEM PRIVATE ${DLSS_SDK_DIR}/include)
  target_link_libraries(rr_test PRIVATE ${DLSS_SDK_DIR}/lib/nvsdk_ngx_s.lib d3d12 dxgi dxguid)
endif()
set_target_properties(FF7RebirthNpcMotion PROPERTIES SUFFIX ".addon64")

# Offline check of the bone register lookup against shaders saved from a capture (tools/rdc_shaderdump.py).
add_executable(reflect_test tests/reflect_test.cpp src/shader_reflection.cpp)
target_include_directories(reflect_test PRIVATE src)
target_link_libraries(reflect_test PRIVATE dxguid)

# Finds the dynamic resolution menu code in the game's executable and checks the redirected loops' values.
add_executable(resolution_menu_test tests/resolution_menu_test.cpp)
target_include_directories(resolution_menu_test PRIVATE src)

# Runs the texture bias routine the way the game enters it.
enable_language(ASM_MASM)
add_executable(bias_routine_test tests/bias_routine_test.cpp tests/bias_routine_test.asm)
target_include_directories(bias_routine_test PRIVATE src)
