cmake_minimum_required(VERSION 3.24)
project(FF7RebirthMods LANGUAGES CXX)

# Every ReShade add-on in one build tree: build\<Mod>\FF7Rebirth<Mod>.addon64 plus each mod's test tools.
# Each folder's own CMakeLists.txt also builds on its own (cmake -S <Mod>). TextureUpscaler is Python and has no build.
add_subdirectory(Anisotropic)
add_subdirectory(FrameLimit)
add_subdirectory(NpcMotion)
add_subdirectory(ShaderInjectorSync)
add_subdirectory(VegetationAndHairFix)

# Investigation tool: samples the thread stacks of the running game (tools/cpu_profile.py reads its output).
add_executable(cpu_sampler tools/cpu_sampler.cpp)
set_target_properties(cpu_sampler PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_definitions(cpu_sampler PRIVATE UNICODE _UNICODE)
target_compile_options(cpu_sampler PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(cpu_sampler PRIVATE dbghelp psapi winmm)

# Development add-on (not shipped): times Shader Injector's passes on the GPU in game (see the file's header).
add_library(FF7RebirthGpuTimer SHARED tools/gpu_timer.cpp)
set_target_properties(FF7RebirthGpuTimer PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON SUFFIX ".addon64"
  MSVC_RUNTIME_LIBRARY "MultiThreaded")
target_compile_definitions(FF7RebirthGpuTimer PRIVATE UNICODE _UNICODE)
target_compile_options(FF7RebirthGpuTimer PRIVATE /W4 /permissive- /utf-8)
target_include_directories(FF7RebirthGpuTimer PRIVATE third_party/reshade third_party/imgui)
