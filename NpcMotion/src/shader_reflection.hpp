// Shader resource bindings by name, from DXIL through dxcompiler.dll's reflection.
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace npcmotion {

struct Binding {
  uint32_t reg = 0;
  uint32_t space = 0;
};

class ShaderReflection {
 public:
  // Loads dxcompiler.dll from the first directory that has one. The module is never unloaded (DllMain).
  bool Initialize(const std::filesystem::path* directories, size_t count, std::string* error);
  bool initialized() const { return create_ != nullptr; }
  const std::filesystem::path& path() const { return path_; }

  // Finds the registers of two named resources in a DXIL container. False when either is missing.
  bool FindPair(const void* code, size_t size, const char* first, const char* second, Binding* first_binding,
                Binding* second_binding) const;

 private:
  using CreateInstanceProc = long(__stdcall*)(const void* clsid, const void* iid, void** object);
  CreateInstanceProc create_ = nullptr;
  std::filesystem::path path_;
};

}  // namespace npcmotion
