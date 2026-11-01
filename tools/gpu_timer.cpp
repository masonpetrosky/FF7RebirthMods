// FF7 Rebirth GPU Timer: a development add-on (not shipped) that times Shader Injector's passes on the GPU in game.
//
// Each pass the injector replaces is recognised by its shader: the replacement it compiled (modded) or the game's
// original (vanilla, drawn while the injector is switched off with Delete). Both lists come from the injector's own
// folders: ModifiedShaders\GameVersion1005_<pass>\*_Compiled.blob and ShaderTargets\*\OriginalShaderBytecode.bin.
// Every draw or dispatch of such a pipeline gets a GPU timestamp right before and right after it. Every second the
// add-on appends the mean GPU time per frame of each pass to FF7RebirthGpuTimer.csv next to it, with the
// QueryPerformanceCounter times of that second (tools/perf_session.py's timeline uses the same clock).
//
// Install for a test only (game closed): copy FF7RebirthGpuTimer.addon64 next to ReShade, delete it afterwards.
// Written 2026-10-31 and built, but not yet run in game: check its ReShade.log lines ("Recognises N modded and M vanilla
// shaders", one "Pipeline for ..." per pass) on its first use.
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" __declspec(dllexport) const char* NAME = "FF7 Rebirth GPU Timer";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Development tool: times Shader Injector's passes on the GPU and logs them to FF7RebirthGpuTimer.csv.";

using namespace reshade::api;

namespace {

namespace fs = std::filesystem;

// Target hash -> pass (the injector's ShaderTargets folders and ModifiedShaders packages of game version 1.005).
const std::map<std::string, std::string> kTargets = {
    {"F6B2E1B7C2560BAB", "ReflectionEnvironment"}, {"397D5B24B450F2A8", "SampleGI"},
    {"FF134B6B13E4E273", "DirectionalLight"},      {"C9B3269F5B403EE2", "LocalLight"},
    {"7266F6A00F286803", "LocalLightIES"},         {"128C0B5CBA1423C7", "SSR"},
    {"2DE9C0E469AC800C", "PostProcessFog"},        {"2CF9A4ABDBE155F7", "OceanA"},
    {"8C15EA09D669BE2A", "WaterA"},                {"95E7477DDFFD2F8E", "WaterB"},
};

constexpr uint32_t kSlots = 16;           // frames in flight in the ring of query slots
constexpr uint32_t kSlotDelay = 8;        // a slot is read this many frames after it was recorded
constexpr uint32_t kPairsPerSlot = 2048;  // timed draws per frame
constexpr uint32_t kMaxLabels = 64;

std::vector<std::string> g_labels;  // "modded SSR", "vanilla SSR", ...
std::unordered_map<uint64_t, uint32_t> g_known;  // (shader length << 32 ^ FNV-1a) -> label

std::shared_mutex g_pipelines_mutex;
std::unordered_map<uint64_t, uint32_t> g_pipelines;  // pipeline -> label

device* g_device = nullptr;
query_heap g_heap = {0};
resource g_readback = {0};  // every timestamp is copied here right after it is written
uint64_t g_frequency = 0;
std::atomic<uint32_t> g_frame{0};
std::array<std::atomic<uint32_t>, kSlots> g_pairs{};                   // pairs allocated per slot
std::array<std::array<uint8_t, kPairsPerSlot>, kSlots> g_pair_label{};  // the label of each pair
std::vector<uint64_t> g_results;

// Per second: summed milliseconds and draws per label over the frames read.
std::array<double, kMaxLabels> g_ms{};
std::array<uint32_t, kMaxLabels> g_draws{};
uint32_t g_frames_read = 0;
int64_t g_second_start = 0;
uint32_t g_dropped = 0;
fs::path g_csv_path;
std::string g_last_line;  // for the overlay

struct __declspec(uuid("6c1e4b47-2f0b-4f0e-9d3a-7a1f5c2e9b40")) CmdState {
  uint32_t label = UINT32_MAX;
};

int64_t Qpc() {
  LARGE_INTEGER v;
  QueryPerformanceCounter(&v);
  return v.QuadPart;
}

uint64_t ShaderKey(const void* code, size_t size) {
  uint64_t h = 1469598103934665603ull;
  const auto* p = static_cast<const uint8_t*>(code);
  for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return (static_cast<uint64_t>(size) << 32) ^ h;
}

uint32_t Label(const std::string& name) {
  for (uint32_t i = 0; i < g_labels.size(); ++i)
    if (g_labels[i] == name) return i;
  g_labels.push_back(name);
  return static_cast<uint32_t>(g_labels.size() - 1);
}

bool AddShader(const fs::path& path, const std::string& label) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (data.empty()) return false;
  g_known[ShaderKey(data.data(), data.size())] = Label(label);
  return true;
}

void LoadShaders(const fs::path& dir) {
  const fs::path injector = dir / "ShaderInjector";
  int modded = 0, vanilla = 0;
  for (const auto& [hash, pass] : kTargets) {
    const std::string type = pass == "ReflectionEnvironment" || pass == "SampleGI" ? "Compute" : "Pixel";
    const fs::path target = injector / "ShaderTargets" / ("ShaderTarget_" + type + "Shader_" + hash);
    vanilla += AddShader(target / "OriginalShaderBytecode.bin", "vanilla " + pass);
    if (type == "Compute") vanilla += AddShader(target / "OriginalComputeShaderBytecode.bin", "vanilla " + pass);
    const std::string package = "GameVersion1005_" + pass;
    modded += AddShader(injector / "ModifiedShaders" / package / (package + "_Compiled.blob"), "modded " + pass);
  }
  reshade::log::message(reshade::log::level::info, ("[GpuTimer] Recognises " + std::to_string(modded) +
                                                    " modded and " + std::to_string(vanilla) + " vanilla shaders.")
                                                       .c_str());
}

void OnInitDevice(device* dev) {
  if (g_device != nullptr) return;
  if (!dev->create_query_heap(query_type::timestamp, kSlots * kPairsPerSlot * 2, &g_heap)) {
    reshade::log::message(reshade::log::level::error, "[GpuTimer] Could not create the query heap.");
    return;
  }
  const uint64_t bytes = uint64_t{kSlots} * kPairsPerSlot * 2 * sizeof(uint64_t);
  if (!dev->create_resource(resource_desc(bytes, memory_heap::readback, resource_usage::copy_dest), nullptr,
                            resource_usage::copy_dest, &g_readback)) {
    reshade::log::message(reshade::log::level::error, "[GpuTimer] Could not create the readback buffer.");
    dev->destroy_query_heap(g_heap);
    return;
  }
  g_device = dev;
  g_results.resize(kPairsPerSlot * 2);
}

void OnDestroyDevice(device* dev) {
  if (dev != g_device) return;
  dev->destroy_query_heap(g_heap);
  dev->destroy_resource(g_readback);
  g_heap = {0};
  g_readback = {0};
  g_device = nullptr;
}

void OnInitCommandQueue(command_queue* queue) {
  if (g_frequency == 0 && (queue->get_type() & command_queue_type::graphics) != 0)
    g_frequency = queue->get_timestamp_frequency();
}

void OnInitPipeline(device*, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects, pipeline pipeline) {
  for (uint32_t i = 0; i < count; ++i) {
    if (subobjects[i].type != pipeline_subobject_type::pixel_shader &&
        subobjects[i].type != pipeline_subobject_type::compute_shader)
      continue;
    const auto* desc = static_cast<const shader_desc*>(subobjects[i].data);
    if (desc == nullptr || desc->code == nullptr) continue;
    const auto it = g_known.find(ShaderKey(desc->code, desc->code_size));
    if (it == g_known.end()) continue;
    std::unique_lock lock(g_pipelines_mutex);
    if (g_pipelines.emplace(pipeline.handle, it->second).second)
      reshade::log::message(reshade::log::level::info, ("[GpuTimer] Pipeline for " + g_labels[it->second]).c_str());
  }
}

void OnDestroyPipeline(device*, pipeline pipeline) {
  std::unique_lock lock(g_pipelines_mutex);
  g_pipelines.erase(pipeline.handle);
}

void OnInitCommandList(command_list* cmd) { cmd->create_private_data<CmdState>(); }
void OnDestroyCommandList(command_list* cmd) { cmd->destroy_private_data<CmdState>(); }
void OnResetCommandList(command_list* cmd) {
  if (auto* st = cmd->get_private_data<CmdState>()) st->label = UINT32_MAX;
}

void OnBindPipeline(command_list* cmd, pipeline_stage, pipeline pipeline) {
  auto* st = cmd->get_private_data<CmdState>();
  if (st == nullptr) return;
  std::shared_lock lock(g_pipelines_mutex);
  const auto it = g_pipelines.find(pipeline.handle);
  st->label = it == g_pipelines.end() ? UINT32_MAX : it->second;
}

// Times one draw or dispatch of a known pass: timestamp, the command itself (issued here), timestamp. Returns
// whether it did, in which case ReShade skips the game's own call.
template <typename Command>
bool Timed(command_list* cmd, Command&& command) {
  if (g_device == nullptr) return false;
  auto* st = cmd->get_private_data<CmdState>();
  if (st == nullptr || st->label == UINT32_MAX) return false;
  const uint32_t slot = g_frame.load(std::memory_order_relaxed) % kSlots;
  const uint32_t pair = g_pairs[slot].fetch_add(1);
  if (pair >= kPairsPerSlot) return false;
  g_pair_label[slot][pair] = static_cast<uint8_t>(st->label);
  const uint32_t first = (slot * kPairsPerSlot + pair) * 2;
  cmd->end_query(g_heap, query_type::timestamp, first);
  command();
  cmd->end_query(g_heap, query_type::timestamp, first + 1);
  cmd->copy_query_heap_results(g_heap, query_type::timestamp, first, 2, g_readback, first * sizeof(uint64_t),
                               sizeof(uint64_t));
  return true;
}

bool OnDraw(command_list* cmd, uint32_t vertices, uint32_t instances, uint32_t first_vertex, uint32_t first_instance) {
  return Timed(cmd, [&] { cmd->draw(vertices, instances, first_vertex, first_instance); });
}

bool OnDrawIndexed(command_list* cmd, uint32_t indices, uint32_t instances, uint32_t first_index, int32_t vertex_offset,
                   uint32_t first_instance) {
  return Timed(cmd, [&] { cmd->draw_indexed(indices, instances, first_index, vertex_offset, first_instance); });
}

bool OnDispatch(command_list* cmd, uint32_t x, uint32_t y, uint32_t z) {
  return Timed(cmd, [&] { cmd->dispatch(x, y, z); });
}

void WriteSecond(int64_t now) {
  if (g_frames_read == 0) return;
  const bool header = !fs::exists(g_csv_path);
  FILE* f = nullptr;
  if (_wfopen_s(&f, g_csv_path.c_str(), L"a") != 0 || f == nullptr) return;
  if (header) std::fprintf(f, "qpc_start,qpc_end,frames,pass,variant,ms_per_frame,draws_per_frame\n");
  std::string line;
  for (uint32_t i = 0; i < g_labels.size(); ++i) {
    if (g_draws[i] == 0) continue;
    const std::string& label = g_labels[i];
    const size_t space = label.find(' ');
    const double ms = g_ms[i] / g_frames_read;
    std::fprintf(f, "%lld,%lld,%u,%s,%s,%.4f,%.2f\n", g_second_start, now, g_frames_read, label.substr(space + 1).c_str(),
                 label.substr(0, space).c_str(), ms, static_cast<double>(g_draws[i]) / g_frames_read);
    char part[96];
    std::snprintf(part, sizeof(part), "%s %.2f ms  ", label.c_str(), ms);
    line += part;
  }
  std::fclose(f);
  g_last_line = line;
  g_ms.fill(0);
  g_draws.fill(0);
  g_frames_read = 0;
}

void OnPresent(command_queue*, swapchain*, const rect*, const rect*, uint32_t, const rect*) {
  if (g_device == nullptr || g_frequency == 0) return;
  const uint32_t frame = g_frame.fetch_add(1) + 1;
  const uint32_t slot = (frame + kSlotDelay) % kSlots;  // recorded kSlots - kSlotDelay frames ago
  const uint32_t pairs = std::min(g_pairs[slot].load(), kPairsPerSlot);
  void* mapped = nullptr;
  const uint64_t offset = uint64_t{slot} * kPairsPerSlot * 2 * sizeof(uint64_t);
  if (frame > kSlots && pairs != 0 &&
      g_device->map_buffer_region(g_readback, offset, pairs * 2 * sizeof(uint64_t), map_access::read_only, &mapped)) {
    std::memcpy(g_results.data(), mapped, pairs * 2 * sizeof(uint64_t));
    g_device->unmap_buffer_region(g_readback);
    for (uint32_t p = 0; p < pairs; ++p) {
      const uint64_t a = g_results[p * 2], b = g_results[p * 2 + 1];
      const double ms = b >= a ? static_cast<double>(b - a) * 1000.0 / static_cast<double>(g_frequency) : -1.0;
      if (ms < 0 || ms > 50) {
        ++g_dropped;
        continue;
      }
      const uint8_t label = g_pair_label[slot][p];
      g_ms[label] += ms;
      ++g_draws[label];
    }
  }
  if (frame > kSlots) ++g_frames_read;
  g_pairs[slot] = 0;
  const int64_t now = Qpc();
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  if (g_second_start == 0) g_second_start = now;
  if (now - g_second_start >= freq.QuadPart) {
    WriteSecond(now);
    g_second_start = now;
  }
}

void OnOverlay(effect_runtime*) {
  ImGui::TextUnformatted(g_labels.empty() ? "No Shader Injector shaders found." : "GPU time per frame, last second:");
  ImGui::TextWrapped("%s", g_last_line.c_str());
  if (g_dropped != 0) ImGui::Text("Dropped timings: %u", g_dropped);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH: {
      if (!reshade::register_addon(module)) return FALSE;
      wchar_t path[MAX_PATH];
      GetModuleFileNameW(module, path, MAX_PATH);
      const fs::path dir = fs::path(path).parent_path();
      g_csv_path = dir / "FF7RebirthGpuTimer.csv";
      LoadShaders(dir);
      reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
      reshade::register_event<reshade::addon_event::init_command_queue>(OnInitCommandQueue);
      reshade::register_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
      reshade::register_event<reshade::addon_event::destroy_pipeline>(OnDestroyPipeline);
      reshade::register_event<reshade::addon_event::init_command_list>(OnInitCommandList);
      reshade::register_event<reshade::addon_event::destroy_command_list>(OnDestroyCommandList);
      reshade::register_event<reshade::addon_event::reset_command_list>(OnResetCommandList);
      reshade::register_event<reshade::addon_event::bind_pipeline>(OnBindPipeline);
      reshade::register_event<reshade::addon_event::draw>(OnDraw);
      reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
      reshade::register_event<reshade::addon_event::dispatch>(OnDispatch);
      reshade::register_event<reshade::addon_event::present>(OnPresent);
      reshade::register_overlay(nullptr, OnOverlay);
      break;
    }
    case DLL_PROCESS_DETACH:
      reshade::unregister_overlay(nullptr, OnOverlay);
      reshade::unregister_addon(module);
      break;
  }
  return TRUE;
}
