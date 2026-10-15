// Runs the texture bias routine (texture_bias_code.hpp) as the game reaches it and checks what it stores, that it
// leaves the flags, xmm6 and xmm8 alone and writes nothing outside View.TemporalSamplerBias.
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "texture_bias_code.hpp"

extern "C" uint64_t RunBiasRoutine(const void* routine, void* view, const float* values, float* registers_after);
extern "C" const void* BiasRoutineReturnAddress();

int main() {
  namespace bias = npcmotion::texture_bias;
  auto* const routine = static_cast<uint8_t*>(
      VirtualAlloc(nullptr, 2 * bias::kConstantOffset, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (routine == nullptr) return 1;
  memcpy(routine, bias::kRoutine, sizeof(bias::kRoutine));
  const uint64_t back = reinterpret_cast<uint64_t>(BiasRoutineReturnAddress());
  memcpy(routine + sizeof(bias::kRoutine), &back, sizeof(back));
  DWORD old;
  if (!VirtualProtect(routine, bias::kConstantOffset, PAGE_EXECUTE_READ, &old)) return 1;
  auto* const constant = reinterpret_cast<float*>(routine + bias::kConstantOffset);

  struct Case {
    const char* name;
    float extra;
    float in[4];
    float want[4];
  };
  const Case cases[] = {
      {"Performance, extra -1", -1.0f, {0.5f, -1.0f, 0.5f, -1.0f}, {0.5f, -2.0f, 0.5f, -2.0f}},
      {"Performance, extra off", 0.0f, {0.5f, -1.0f, 0.5f, -1.0f}, {0.5f, -1.0f, 0.5f, -1.0f}},
      {"Quality, extra -0.5", -0.5f, {0.667f, -0.585f, 0.667f, -0.585f}, {0.667f, -1.085f, 0.667f, -1.085f}},
      {"not upscaled", -1.0f, {1.0f, 0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 1.0f, 0.0f}},
  };
  int failures = 0;
  for (const Case& c : cases) {
    constant[0] = c.extra;
    uint8_t view[0xC60];
    memset(view, 0xAB, sizeof(view));
    float after[2] = {};
    const uint64_t flags = RunBiasRoutine(routine, view, c.in, after);
    float got[4];
    memcpy(got, view + 0xC40, sizeof(got));
    bool ok = flags == 0 && after[0] == c.in[2] && after[1] == c.in[3] && view[0xC3F] == 0xAB && view[0xC50] == 0xAB;
    for (int i = 0; i < 4; ++i) ok = ok && std::fabs(got[i] - c.want[i]) < 1e-6f;
    printf("%s %-24s (%g, %g, %g, %g) -> (%g, %g, %g, %g); flags changed 0x%llx; xmm6 %g xmm8 %g\n",
           ok ? "ok  " : "FAIL", c.name, c.in[0], c.in[1], c.in[2], c.in[3], got[0], got[1], got[2], got[3],
           static_cast<unsigned long long>(flags), after[0], after[1]);
    failures += ok ? 0 : 1;
  }
  return failures;
}
