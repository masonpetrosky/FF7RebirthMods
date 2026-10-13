// FF7 Rebirth NPC Motion
// ReShade add-on that fixes the motion vectors of characters whose animation the game did not update this frame.
//
// FINAL FANTASY VII REBIRTH (UE 4.26) throttles character animation with the animation budget allocator
// (a.Budget.MaxTickRate=3, no interpolation), so a townsperson's pose is often updated only every second or third
// frame. A skinned mesh keeps two GPU bone buffers, and on frames without an update UE 4.26's GPU skin vertex factory
// still binds the older one as PreviousBoneMatrices with PerBoneMotionBlur on: the velocity the base pass writes
// repeats the last update's motion while the pose stands still. DLSS (and the game's TAA and motion blur) then
// reproject those characters from the wrong place, which shows as blurry edges. UE 4.27 binds the current bones as
// the previous ones in that case; this add-on does the same at the D3D12 level.
//
// The game gives every pose update a new bone buffer, so a draw whose BoneMatrices buffer was some draw's
// BoneMatrices in the previous frame has not been updated: its PreviousBoneMatrices descriptor is overwritten with
// the BoneMatrices one in the draw's own descriptor table. Skinning motion becomes zero for that frame; the
// character's movement through the world (its transform) still produces motion vectors.
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "descriptors.hpp"
#include "dlss_probe.hpp"
#include "shader_reflection.hpp"

extern "C" __declspec(dllexport) const char* NAME = "FF7 Rebirth NPC Motion";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Fixes the motion vectors of characters whose animation FINAL FANTASY VII REBIRTH skipped this frame, so DLSS "
    "no longer blurs their edges.";

using namespace reshade::api;

namespace {

namespace fs = std::filesystem;

constexpr char kCurrentBones[] = "BoneMatrices";
constexpr char kPreviousBones[] = "PreviousBoneMatrices";

fs::path g_ini_path;
std::atomic<bool> g_enabled{true};
int g_toggle_key = 0;  // virtual-key code, 0 = none
bool g_log_stats = false;
// 1 = the game's animation budget; 0 = switched off at startup, so every character animates every frame.
int g_animation_budget = 1;
// Characters the budget may interpolate on the frames it skips (a.Budget[For60FPS].MaxInterpolatedComponents,
// which the game sets to 0; UE's default is 16). -1 = leave the game's value.
int g_interpolated_characters = -1;
std::string g_budget_status = "the game's";

npcmotion::ShaderReflection g_reflection;
std::string g_reflection_error;

// Graphics pipelines whose vertex shader reads PreviousBoneMatrices, with the registers of both bone buffers.
struct SkinRegisters {
  npcmotion::Binding current, previous;
  // The View uniform buffer and the byte offset of View_TemporalAAJitter in it (DLSS probe).
  npcmotion::Binding view;
  uint32_t jitter_offset = 0;
  bool has_view = false;
};
std::shared_mutex g_pipelines_mutex;
std::unordered_map<uint64_t, SkinRegisters> g_skin_pipelines;
// Vertex shaders seen so far, by a hash of their bytecode (many pipelines share one): registers, or none.
std::mutex g_shaders_mutex;
std::unordered_map<uint64_t, std::optional<SkinRegisters>> g_shaders;
std::atomic<size_t> g_reflection_failures{0};

// A bone buffer as the draws see it: the buffer resource and the byte offset its view starts at.
struct BoneBuffer {
  uint64_t resource = 0;
  uint64_t offset = 0;
  bool operator==(const BoneBuffer&) const = default;
};
struct BoneBufferHash {
  size_t operator()(const BoneBuffer& b) const {
    return std::hash<uint64_t>()(b.resource) ^ (std::hash<uint64_t>()(b.offset) * 0x9E3779B97F4A7C15ull);
  }
};

// BoneMatrices buffers used in this frame and in the previous one (swapped at present).
std::mutex g_frame_mutex;
std::unordered_set<BoneBuffer, BoneBufferHash> g_this_frame, g_last_frame;

// Statistics: counters for the frame being recorded and the last complete frame.
struct Stats {
  uint32_t draws = 0;       // draws whose vertex shader reads PreviousBoneMatrices
  uint32_t fixed = 0;       // animation not updated this frame: previous bones replaced by the current ones
  uint32_t updated = 0;     // animation updated: left as the game bound it
  uint32_t same = 0;        // the game already bound the same buffer twice
  uint32_t unresolved = 0;  // a bone buffer descriptor could not be looked up
};
std::atomic<uint32_t> g_draws{0}, g_fixed{0}, g_updated{0}, g_same{0}, g_unresolved{0};
Stats g_last_stats;  // written at present, read by the overlay
std::atomic<uint64_t> g_frame{0};

// Per command list: the bound graphics pipeline (if it skins with previous bones), root signature and tables.
struct __declspec(uuid("9b5d3c21-7e64-4f0a-8c1d-2a6e9f4b7d38")) CommandListData {
  bool skinned = false;
  SkinRegisters registers;
  pipeline_layout layout = {0};
  std::vector<descriptor_table> tables;  // index = root parameter of `layout`
  std::vector<buffer_range> root_cbvs;   // root constant buffers, index = root parameter of `layout`
};

// Presents so far (every present, including frames DLSS frame generation adds); a frame's draws carry the count
// of the presents before them.
std::atomic<uint64_t> g_presents{0};
int g_probe_key = 0;

void Log(reshade::log::level level, const std::string& message) {
  reshade::log::message(level, ("[NpcMotion] " + message).c_str());
}

// ---------------------------------------------------------------------------------------------
// Configuration

int ReadIniInt(const wchar_t* key, int fallback) {
  return static_cast<int>(GetPrivateProfileIntW(L"NpcMotion", key, fallback, g_ini_path.c_str()));
}

void WriteIniInt(const wchar_t* key, int value) {
  WritePrivateProfileStringW(L"NpcMotion", key, std::to_wstring(value).c_str(), g_ini_path.c_str());
}

void SetEnabled(bool enabled) {
  g_enabled = enabled;
  WriteIniInt(L"Enabled", enabled ? 1 : 0);
  Log(reshade::log::level::info, enabled ? "Fix on." : "Fix off.");
}

// ---------------------------------------------------------------------------------------------
// Animation budget

// Finds the variable behind an FAutoConsoleVariableRef in the game's executable. The code that registers it loads the
// variable's address into r8 (lea r8, [rip+X]) shortly before the name into rdx (lea rdx, [rip+name]).
void* FindConsoleVariable(const wchar_t* name) {
  auto* const base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
  const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
  const uint8_t *text = nullptr, *rdata = nullptr;
  size_t text_size = 0, rdata_size = 0;
  for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    const char* section = reinterpret_cast<const char*>(sections[i].Name);
    if (strncmp(section, ".text", 8) == 0) text = base + sections[i].VirtualAddress, text_size = sections[i].Misc.VirtualSize;
    if (strncmp(section, ".rdata", 8) == 0)
      rdata = base + sections[i].VirtualAddress, rdata_size = sections[i].Misc.VirtualSize;
  }
  if (text == nullptr || rdata == nullptr) return nullptr;
  const size_t name_bytes = (wcslen(name) + 1) * sizeof(wchar_t);  // with the terminator: the exact name only
  std::vector<const uint8_t*> strings;
  for (size_t i = 0; i + name_bytes <= rdata_size; i += 2)
    if (memcmp(rdata + i, name, name_bytes) == 0) strings.push_back(rdata + i);
  for (size_t i = 0; i + 7 <= text_size; ++i) {
    if (text[i] != 0x48 || text[i + 1] != 0x8D || text[i + 2] != 0x15) continue;  // lea rdx, [rip+disp32]
    int32_t disp;
    memcpy(&disp, text + i + 3, 4);
    const uint8_t* target = text + i + 7 + disp;
    if (std::find(strings.begin(), strings.end(), target) == strings.end()) continue;
    for (size_t back = 7; back <= 48 && back <= i; ++back) {
      const uint8_t* p = text + i - back;
      if (p[0] != 0x4C || p[1] != 0x8D || p[2] != 0x05) continue;  // lea r8, [rip+disp32]
      memcpy(&disp, p + 3, 4);
      return const_cast<uint8_t*>(p + 7 + disp);
    }
  }
  return nullptr;
}

// Writes an integer console variable before the world exists, after checking that it holds a plausible value.
bool SetIntVariable(const wchar_t* name, int32_t lo, int32_t hi, int32_t value, int32_t* before) {
  auto* const variable = static_cast<int32_t*>(FindConsoleVariable(name));
  if (variable == nullptr || *variable < lo || *variable > hi) return false;
  *before = *variable;
  *variable = value;
  return true;
}

// The budget allocator copies a.Budget.* (or a.BudgetFor60FPS.*, the game picks one per allocator) into its own
// parameters when it is set up, so both are changed before any world exists.
void ApplyInterpolation() {
  if (g_interpolated_characters < 0) return;
  int32_t before = 0, before60 = 0;
  const bool a = SetIntVariable(L"a.Budget.MaxInterpolatedComponents", 0, 100000, g_interpolated_characters, &before);
  const bool b =
      SetIntVariable(L"a.BudgetFor60FPS.MaxInterpolatedComponents", 0, 100000, g_interpolated_characters, &before60);
  if (a && b) {
    g_budget_status = "on, interpolating up to " + std::to_string(g_interpolated_characters) + " characters";
    Log(reshade::log::level::info, "Animation budget interpolates up to " + std::to_string(g_interpolated_characters) +
                                       " characters (the game's values were " + std::to_string(before) + " and " +
                                       std::to_string(before60) + ").");
  } else {
    g_budget_status = "interpolation could not be set";
    Log(reshade::log::level::warning, "Animation budget: MaxInterpolatedComponents not found or unexpected.");
  }
}

// Switches the animation budget allocator off before any character exists (UE: "should be set when there are no
// running skeletal meshes"). FAnimationBudgetAllocator::Update skips all budgeting while a.Budget.Enabled is not 1.
void ApplyAnimationBudget() {
  ApplyInterpolation();
  if (g_animation_budget != 0) return;
  auto* const enabled = static_cast<int32_t*>(FindConsoleVariable(L"a.Budget.Enabled"));
  if (enabled == nullptr) {
    g_budget_status = "could not be switched off (a.Budget.Enabled not found)";
    Log(reshade::log::level::warning, "Animation budget: a.Budget.Enabled not found; left as the game set it.");
    return;
  }
  if (*enabled != 0 && *enabled != 1) {
    g_budget_status = "could not be switched off (unexpected value)";
    Log(reshade::log::level::warning, "Animation budget: unexpected a.Budget.Enabled value " + std::to_string(*enabled) +
                                          "; left as the game set it.");
    return;
  }
  const int32_t before = *enabled;
  *enabled = 0;
  g_budget_status = "off: every character animates every frame";
  Log(reshade::log::level::info, "Animation budget switched off (a.Budget.Enabled was " + std::to_string(before) + ").");
}

void OnInitDevice(device*) {
  static std::once_flag once;
  std::call_once(once, ApplyAnimationBudget);
}

// ---------------------------------------------------------------------------------------------
// Pipelines

uint64_t Fnv1a(const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint64_t hash = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 0x100000001B3ull;
  return hash ^ size;
}

bool Contains(const void* data, size_t size, const char* text) {
  const size_t length = strlen(text);
  const auto* bytes = static_cast<const char*>(data);
  for (size_t i = 0; i + length <= size; ++i)
    if (bytes[i] == text[0] && memcmp(bytes + i, text, length) == 0) return true;
  return false;
}

std::optional<SkinRegisters> InspectVertexShader(const shader_desc& vs) {
  const uint64_t hash = Fnv1a(vs.code, vs.code_size);
  {
    std::lock_guard lock(g_shaders_mutex);
    if (const auto it = g_shaders.find(hash); it != g_shaders.end()) return it->second;
  }
  std::optional<SkinRegisters> result;
  // The resource names are in the DXIL metadata, so most shaders are ruled out without reflection.
  if (Contains(vs.code, vs.code_size, kPreviousBones)) {
    SkinRegisters regs;
    if (g_reflection.FindPair(vs.code, vs.code_size, kCurrentBones, kPreviousBones, &regs.current, &regs.previous)) {
      regs.has_view = g_reflection.FindConstant(vs.code, vs.code_size, "View", "View_TemporalAAJitter", &regs.view,
                                                &regs.jitter_offset);
      result = regs;
    }
    else
      ++g_reflection_failures;
  }
  std::lock_guard lock(g_shaders_mutex);
  g_shaders.emplace(hash, result);
  return result;
}

void OnInitPipeline(device*, pipeline_layout, uint32_t subobject_count, const pipeline_subobject* subobjects,
                    pipeline pipeline) {
  for (uint32_t i = 0; i < subobject_count; ++i) {
    if (subobjects[i].type != pipeline_subobject_type::vertex_shader) continue;
    const auto* vs = static_cast<const shader_desc*>(subobjects[i].data);
    if (vs == nullptr || vs->code == nullptr || vs->code_size == 0) return;
    if (const std::optional<SkinRegisters> regs = InspectVertexShader(*vs)) {
      std::unique_lock lock(g_pipelines_mutex);
      g_skin_pipelines[pipeline.handle] = *regs;
    }
    return;
  }
}

void OnDestroyPipeline(device*, pipeline pipeline) {
  std::unique_lock lock(g_pipelines_mutex);
  g_skin_pipelines.erase(pipeline.handle);
}

// ---------------------------------------------------------------------------------------------
// Command lists

void OnInitCommandList(command_list* cmd_list) { cmd_list->create_private_data<CommandListData>(); }

void OnDestroyCommandList(command_list* cmd_list) { cmd_list->destroy_private_data<CommandListData>(); }

void OnResetCommandList(command_list* cmd_list) {
  auto* data = cmd_list->get_private_data<CommandListData>();
  if (data == nullptr) return;
  data->skinned = false;
  data->layout = {0};
  data->tables.clear();
  data->root_cbvs.clear();
}

void OnBindPipeline(command_list* cmd_list, pipeline_stage stages, pipeline pipeline) {
  if ((stages & pipeline_stage::vertex_shader) == 0) return;
  auto* data = cmd_list->get_private_data<CommandListData>();
  if (data == nullptr) return;
  std::shared_lock lock(g_pipelines_mutex);
  const auto it = g_skin_pipelines.find(pipeline.handle);
  data->skinned = it != g_skin_pipelines.end();
  if (data->skinned) data->registers = it->second;
}

void OnBindDescriptorTables(command_list* cmd_list, shader_stage stages, pipeline_layout layout, uint32_t first,
                            uint32_t count, const descriptor_table* tables, uint32_t, const uint32_t*) {
  if ((stages & shader_stage::vertex) == 0) return;
  auto* data = cmd_list->get_private_data<CommandListData>();
  if (data == nullptr) return;
  if (data->layout != layout) {
    data->layout = layout;
    data->tables.clear();
    data->root_cbvs.clear();
  }
  if (data->tables.size() < first + count) data->tables.resize(first + count);
  for (uint32_t i = 0; i < count; ++i) data->tables[first + i] = tables[i];
}

void OnPushDescriptors(command_list* cmd_list, shader_stage stages, pipeline_layout layout, uint32_t param,
                       const descriptor_table_update& update) {
  if ((stages & shader_stage::vertex) == 0 || update.type != descriptor_type::constant_buffer || update.count == 0 ||
      !npcmotion::dlss_probe::enabled())
    return;
  auto* data = cmd_list->get_private_data<CommandListData>();
  if (data == nullptr) return;
  if (data->layout != layout) {
    data->layout = layout;
    data->tables.clear();
    data->root_cbvs.clear();
  }
  if (data->root_cbvs.size() <= param) data->root_cbvs.resize(param + 1);
  data->root_cbvs[param] = static_cast<const buffer_range*>(update.descriptors)[0];
}

// DLSS probe: reads View_TemporalAAJitter from the View buffer bound to this skinned draw (the first of a frame).
void CaptureProjectionJitter(device* dev, const npcmotion::Descriptors& descriptors, const CommandListData& data) {
  const uint64_t frame = g_presents.load();
  if (!data.registers.has_view || npcmotion::dlss_probe::HasProjectionJitter(frame)) return;
  uint32_t param = 0;
  if (!descriptors.LocateRootConstantBuffer(data.layout, shader_stage::vertex, data.registers.view.reg,
                                            data.registers.view.space, &param) ||
      param >= data.root_cbvs.size() || data.root_cbvs[param].buffer == 0)
    return;
  const buffer_range& range = data.root_cbvs[param];
  void* mapped = nullptr;
  if (!dev->map_buffer_region(range.buffer, range.offset + data.registers.jitter_offset, 16, map_access::read_only,
                              &mapped) ||
      mapped == nullptr)
    return;
  float jitter[4];
  memcpy(jitter, mapped, sizeof(jitter));
  dev->unmap_buffer_region(range.buffer);
  npcmotion::dlss_probe::SetProjectionJitter(frame, jitter);
}

// Looks up the view in a vertex-stage shader register and the bone buffer it shows.
bool ResolveBones(device* dev, const npcmotion::Descriptors& descriptors, const CommandListData& data,
                  const npcmotion::Binding& reg, npcmotion::Descriptors::Slot* slot, resource_view* view,
                  BoneBuffer* buffer) {
  if (!descriptors.LocateShaderResource(data.layout, data.tables, shader_stage::vertex, reg.reg, reg.space, slot))
    return false;
  descriptor_heap heap = {0};
  uint32_t offset = 0;
  dev->get_descriptor_heap_offset(data.tables[slot->param], slot->binding, 0, &heap, &offset);
  *view = descriptors.View(heap, offset);
  if (*view == 0) return false;
  const resource res = dev->get_resource_from_view(*view);
  if (res == 0) return false;
  const resource_view_desc desc = dev->get_resource_view_desc(*view);
  buffer->resource = res.handle;
  buffer->offset = desc.buffer.offset;
  return true;
}

void FixDraw(command_list* cmd_list) {
  auto* data = cmd_list->get_private_data<CommandListData>();
  if (data == nullptr || !data->skinned) return;
  device* const dev = cmd_list->get_device();
  const auto* descriptors = dev->get_private_data<npcmotion::Descriptors>();
  if (descriptors == nullptr) return;
  ++g_draws;
  if (npcmotion::dlss_probe::enabled()) CaptureProjectionJitter(dev, *descriptors, *data);

  npcmotion::Descriptors::Slot current_slot, previous_slot;
  resource_view current_view = {0}, previous_view = {0};
  BoneBuffer current, previous;
  if (!ResolveBones(dev, *descriptors, *data, data->registers.current, &current_slot, &current_view, &current) ||
      !ResolveBones(dev, *descriptors, *data, data->registers.previous, &previous_slot, &previous_view, &previous)) {
    ++g_unresolved;
    return;
  }
  bool stale;
  {
    std::lock_guard lock(g_frame_mutex);
    g_this_frame.insert(current);
    stale = !(current == previous) && g_last_frame.count(current) != 0;
  }
  if (current == previous) {
    ++g_same;
    return;
  }
  if (!stale) {
    ++g_updated;
    return;
  }
  ++g_fixed;
  if (!g_enabled) return;
  const descriptor_table_update update = {data->tables[previous_slot.param], previous_slot.binding, 0, 1,
                                          descriptor_type::shader_resource_view, &current_view};
  dev->update_descriptor_tables(1, &update);
}

bool OnDraw(command_list* cmd_list, uint32_t, uint32_t, uint32_t, uint32_t) {
  FixDraw(cmd_list);
  return false;
}

bool OnDrawIndexed(command_list* cmd_list, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) {
  FixDraw(cmd_list);
  return false;
}

// ---------------------------------------------------------------------------------------------
// Frames, keys and overlay

bool KeyPressed(int vk, bool& was_down) {
  if (vk == 0) return false;
  const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
  const bool pressed = down && !was_down;
  was_down = down;
  return pressed;
}

void OnPresent(command_queue*, swapchain*, const rect*, const rect*, uint32_t, const rect*) {
  npcmotion::dlss_probe::OnPresent(++g_presents);
  static bool probe_down = false;
  if (KeyPressed(g_probe_key, probe_down) && npcmotion::dlss_probe::enabled()) npcmotion::dlss_probe::Arm(120);
  // Presents without skinned draws (frames generated by DLSS frame generation, menus, loading screens) keep the last
  // rendered frame's buffers; after a longer stretch of them those are forgotten, since their memory may be reused.
  static uint32_t presents_without_draws = 0;
  {
    std::lock_guard lock(g_frame_mutex);
    if (!g_this_frame.empty()) {
      std::swap(g_this_frame, g_last_frame);
      g_this_frame.clear();
      presents_without_draws = 0;
    } else if (++presents_without_draws == 8) {
      g_last_frame.clear();
    }
  }
  if (g_draws.load() == 0 && presents_without_draws != 0 && presents_without_draws < 8) return;
  g_last_stats = {g_draws.exchange(0), g_fixed.exchange(0), g_updated.exchange(0), g_same.exchange(0),
                  g_unresolved.exchange(0)};
  const uint64_t frame = ++g_frame;

  static bool toggle_down = false;
  if (KeyPressed(g_toggle_key, toggle_down)) SetEnabled(!g_enabled);

  if (g_log_stats && frame % 600 == 0) {
    static auto last_time = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - last_time).count();
    last_time = now;
    size_t pipelines;
    {
      std::shared_lock lock(g_pipelines_mutex);
      pipelines = g_skin_pipelines.size();
    }
    char line[320];
    snprintf(line, sizeof(line),
             "frame %llu (%.1f fps): %u skinned draws, %u not updated (%s), %u updated, %u already still, "
             "%u unresolved; %zu skinned pipelines; animation budget %s",
             static_cast<unsigned long long>(frame), seconds > 0.5 ? 600.0 / seconds : 0.0, g_last_stats.draws,
             g_last_stats.fixed, g_enabled ? "fixed" : "fix off", g_last_stats.updated, g_last_stats.same,
             g_last_stats.unresolved, pipelines, g_animation_budget != 0 ? "on" : "off");
    Log(reshade::log::level::info, line);
  }
}

void OnOverlay(effect_runtime*) {
  bool enabled = g_enabled;
  if (ImGui::Checkbox("Fix motion vectors of characters with skipped animation", &enabled)) SetEnabled(enabled);
  ImGui::SetItemTooltip(
      "On frames where the game did not update a character's animation, its motion vectors repeat the last\n"
      "update's movement. With this on, they only carry the character's movement through the world.");
  if (g_toggle_key != 0) ImGui::Text("Toggle key: virtual-key %d", g_toggle_key);

  ImGui::Spacing();
  const Stats s = g_last_stats;
  ImGui::Text("Last frame: %u skinned draws", s.draws);
  ImGui::Text("  %u not updated this frame (%s)", s.fixed, g_enabled ? "fixed" : "left stale, fix off");
  ImGui::Text("  %u updated, %u already without skinning motion", s.updated, s.same);
  if (s.unresolved != 0) ImGui::Text("  %u could not be looked up", s.unresolved);
  size_t pipelines;
  {
    std::shared_lock lock(g_pipelines_mutex);
    pipelines = g_skin_pipelines.size();
  }
  ImGui::Text("Skinned pipelines with previous bones: %zu", pipelines);
  ImGui::Text("Animation budget: %s (AnimationBudget in the ini, applies at the next start)", g_budget_status.c_str());
  if (!g_reflection.initialized()) ImGui::Text("Not working: %s", g_reflection_error.c_str());
  if (npcmotion::dlss_probe::enabled()) {
    ImGui::Text("DLSS probe: %s", npcmotion::dlss_probe::Status().c_str());
    if (ImGui::Button("Log the next 120 DLSS evaluations")) npcmotion::dlss_probe::Arm(120);
  }
  if (g_reflection_failures != 0)
    ImGui::Text("Shaders whose bone registers could not be read: %zu (see ReShade.log)", g_reflection_failures.load());
}

void Initialize(HMODULE module) {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(module, path, MAX_PATH);
  const fs::path dir = fs::path(path).parent_path();
  g_ini_path = dir / "FF7RebirthNpcMotion.ini";
  g_enabled = ReadIniInt(L"Enabled", 1) != 0;
  g_toggle_key = ReadIniInt(L"ToggleKey", 0);
  g_log_stats = ReadIniInt(L"LogStats", 0) != 0;
  g_animation_budget = ReadIniInt(L"AnimationBudget", 1);
  g_interpolated_characters = ReadIniInt(L"InterpolatedCharacters", -1);
  if (ReadIniInt(L"DlssProbe", 0) != 0) {
    g_probe_key = ReadIniInt(L"DlssProbeKey", 117);  // F6
    npcmotion::dlss_probe::Initialize(dir / "FF7RebirthNpcMotion_dlss.log");
  }
  const fs::path dll_dirs[] = {dir / "FF7RebirthNpcMotion", dir / "FF7RebirthHairFix"};
  if (g_reflection.Initialize(dll_dirs, std::size(dll_dirs), &g_reflection_error))
    Log(reshade::log::level::info, "Using " + g_reflection.path().string() + (g_enabled ? "; fix on." : "; fix off."));
  else
    Log(reshade::log::level::error, "Not working: " + g_reflection_error);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(module)) return FALSE;
      Initialize(module);
      npcmotion::Descriptors::RegisterEvents();
      reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::register_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
      reshade::register_event<reshade::addon_event::destroy_pipeline>(OnDestroyPipeline);
      reshade::register_event<reshade::addon_event::init_command_list>(OnInitCommandList);
      reshade::register_event<reshade::addon_event::destroy_command_list>(OnDestroyCommandList);
      reshade::register_event<reshade::addon_event::reset_command_list>(OnResetCommandList);
      reshade::register_event<reshade::addon_event::bind_pipeline>(OnBindPipeline);
      reshade::register_event<reshade::addon_event::bind_descriptor_tables>(OnBindDescriptorTables);
      reshade::register_event<reshade::addon_event::push_descriptors>(OnPushDescriptors);
      reshade::register_event<reshade::addon_event::draw>(OnDraw);
      reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
      reshade::register_event<reshade::addon_event::present>(OnPresent);
      reshade::register_overlay(nullptr, OnOverlay);
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_overlay(nullptr, OnOverlay);
      reshade::unregister_event<reshade::addon_event::init_device>(OnInitDevice);
      npcmotion::Descriptors::UnregisterEvents();
      reshade::unregister_addon(module);
      break;
  }
  return TRUE;
}
