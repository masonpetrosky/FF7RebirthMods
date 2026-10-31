cmake_minimum_required(VERSION 3.24)
project(FF7RebirthMods LANGUAGES CXX)

# Every ReShade add-on in one build tree: build\<Mod>\FF7Rebirth<Mod>.addon64 plus each mod's test tools.
# Each folder's own CMakeLists.txt also builds on its own (cmake -S <Mod>). TextureUpscaler is Python and has no build.
add_subdirectory(Anisotropic)
add_subdirectory(NpcMotion)
add_subdirectory(ShaderInjectorSync)
add_subdirectory(VegetationAndHairFix)
