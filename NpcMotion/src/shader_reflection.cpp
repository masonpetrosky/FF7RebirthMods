// SPDX-License-Identifier: MIT

#include "shader_reflection.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d12shader.h>
#include <dxcapi.h>

namespace npcmotion {
namespace {

template <typename T>
struct ComPtr {
  T* p = nullptr;
  ~ComPtr() {
    if (p) p->Release();
  }
  T** operator&() { return &p; }
  T* operator->() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

}  // namespace

bool ShaderReflection::Initialize(const std::filesystem::path* directories, size_t count, std::string* error) {
  for (size_t i = 0; i < count; ++i) {
    const std::filesystem::path dll = directories[i] / L"dxcompiler.dll";
    if (!std::filesystem::exists(dll)) continue;
    HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) continue;
    create_ = reinterpret_cast<CreateInstanceProc>(GetProcAddress(module, "DxcCreateInstance"));
    if (create_ != nullptr) {
      path_ = dll;
      return true;
    }
  }
  *error = "dxcompiler.dll not found next to the add-on (FF7RebirthNpcMotion or FF7RebirthHairFix folder)";
  return false;
}

bool ShaderReflection::FindPair(const void* code, size_t size, const char* first, const char* second,
                                Binding* first_binding, Binding* second_binding) const {
  if (create_ == nullptr) return false;
  ComPtr<IDxcUtils> utils;
  if (FAILED(create_(&CLSID_DxcUtils, &__uuidof(IDxcUtils), reinterpret_cast<void**>(&utils)))) return false;
  const DxcBuffer buffer = {code, size, DXC_CP_ACP};
  ComPtr<ID3D12ShaderReflection> reflection;
  if (FAILED(utils->CreateReflection(&buffer, __uuidof(ID3D12ShaderReflection), reinterpret_cast<void**>(&reflection))))
    return false;
  D3D12_SHADER_INPUT_BIND_DESC a = {}, b = {};
  if (FAILED(reflection->GetResourceBindingDescByName(first, &a)) ||
      FAILED(reflection->GetResourceBindingDescByName(second, &b)))
    return false;
  first_binding->reg = a.BindPoint;
  first_binding->space = a.Space;
  second_binding->reg = b.BindPoint;
  second_binding->space = b.Space;
  return true;
}

}  // namespace npcmotion
