// Runs the add-on's bone register lookup on shader files: reflect_test <dir with dxcompiler.dll> <shader.bin>...
// SPDX-License-Identifier: MIT

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "shader_reflection.hpp"

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) {
    fwprintf(stderr, L"usage: reflect_test <dxcompiler dir> <shader.bin>...\n");
    return 2;
  }
  npcmotion::ShaderReflection reflection;
  std::string error;
  const std::filesystem::path dirs[] = {argv[1]};
  if (!reflection.Initialize(dirs, 1, &error)) {
    fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  int failures = 0;
  for (int i = 2; i < argc; ++i) {
    std::ifstream file(argv[i], std::ios::binary);
    const std::vector<char> code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    npcmotion::Binding current, previous;
    const bool ok = reflection.FindPair(code.data(), code.size(), "BoneMatrices", "PreviousBoneMatrices", &current,
                                        &previous);
    npcmotion::Binding view;
    uint32_t jitter_offset = 0;
    const bool has_view =
        reflection.FindConstant(code.data(), code.size(), "View", "View_TemporalAAJitter", &view, &jitter_offset);
    if (ok)
      wprintf(L"%s: BoneMatrices t%u space%u, PreviousBoneMatrices t%u space%u, View %s b%u space%u, jitter at %u\n",
              argv[i], current.reg, current.space, previous.reg, previous.space, has_view ? L"found" : L"missing",
              view.reg, view.space, jitter_offset);
    else
      wprintf(L"%s: not found\n", argv[i]);
    failures += ok ? 0 : 1;
  }
  return failures;
}
