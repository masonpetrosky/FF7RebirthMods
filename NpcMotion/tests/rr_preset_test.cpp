// Offline check of the Ray Reconstruction preset choice (src/rr_preset.hpp): the ini's text, and Auto on card names as
// DXGI gives them (GeForce, laptop, workstation and Titan names of each generation).
// SPDX-License-Identifier: MIT

#include <cstdio>
#include <string>

#include "rr_preset.hpp"

int main() {
  using namespace npcmotion::rr;
  int wrong = 0;
  const auto expect = [&](const char* name, char letter) {
    std::string why;
    const char got = PresetLetter(AutoPreset(name, &why));
    if (got != letter) ++wrong;
    std::printf("  %s %-44s -> %c (%s)\n", got == letter ? "ok  " : "FAIL", name, got, why.c_str());
  };
  expect("NVIDIA GeForce RTX 5070 Ti", 'F');
  expect("NVIDIA GeForce RTX 5090", 'F');
  expect("NVIDIA GeForce RTX 4090", 'F');
  expect("NVIDIA GeForce RTX 4060 Laptop GPU", 'F');
  expect("NVIDIA RTX 2000 Ada Generation", 'F');
  expect("NVIDIA RTX 6000 Ada Generation", 'F');
  expect("NVIDIA RTX PRO 6000 Blackwell Workstation Edition", 'F');
  expect("NVIDIA GeForce RTX 3090", 'D');
  expect("NVIDIA GeForce RTX 3060 Laptop GPU", 'D');
  expect("NVIDIA GeForce RTX 2080 Ti", 'D');
  expect("NVIDIA GeForce RTX 2060 SUPER", 'D');
  expect("NVIDIA RTX A4000", 'D');
  expect("NVIDIA RTX A2000 12GB", 'D');
  expect("Quadro RTX 6000", 'D');
  expect("NVIDIA TITAN RTX", 'D');
  expect("Some future card", 'F');
  const struct {
    const char* text;
    uint32_t preset;
  } texts[] = {{"Auto", kPresetAuto}, {"", kPresetAuto}, {"d", kPresetD}, {"D", kPresetD},
               {"E", kPresetE},       {"F", kPresetF},    {"f", kPresetF}, {"G", kPresetAuto}};
  for (const auto& t : texts) {
    const bool ok = PresetFromText(t.text) == t.preset;
    wrong += ok ? 0 : 1;
    std::printf("  %s the ini's \"%s\" -> %u\n", ok ? "ok  " : "FAIL", t.text, PresetFromText(t.text));
  }
  std::printf(wrong == 0 ? "PASS\n" : "FAIL\n");
  return wrong == 0 ? 0 : 1;
}
