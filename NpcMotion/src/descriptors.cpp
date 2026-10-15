// SPDX-License-Identifier: MIT

#include "descriptors.hpp"

using namespace reshade::api;

namespace npcmotion {
namespace {

bool IsShaderResource(descriptor_type type) {
  return type == descriptor_type::shader_resource_view || type == descriptor_type::buffer_shader_resource_view;
}

bool IsTable(pipeline_layout_param_type type) {
  return type == pipeline_layout_param_type::descriptor_table ||
         type == pipeline_layout_param_type::descriptor_table_with_flags;
}

// Root descriptors. ReShade reports those of a version 1.1 root signature (which carry flags, as the game's do) as
// push_descriptors_with_ranges_and_flags, with one range.
bool IsPush(pipeline_layout_param_type type) {
  return type == pipeline_layout_param_type::push_descriptors ||
         type == pipeline_layout_param_type::push_descriptors_with_ranges ||
         type == pipeline_layout_param_type::push_descriptors_with_ranges_and_flags;
}

const char* TypeName(descriptor_type type) {
  switch (type) {
    case descriptor_type::sampler: return "sampler";
    case descriptor_type::sampler_with_resource_view: return "sampler+srv";
    case descriptor_type::shader_resource_view: return "srv";
    case descriptor_type::unordered_access_view: return "uav";
    case descriptor_type::buffer_shader_resource_view: return "buffer-srv";
    case descriptor_type::buffer_unordered_access_view: return "buffer-uav";
    case descriptor_type::constant_buffer: return "cbv";
    default: return "other";
  }
}

std::string Stages(shader_stage stages) {
  if (stages == shader_stage::all) return "all";
  std::string out;
  if ((stages & shader_stage::vertex) != 0) out += "vs";
  if ((stages & shader_stage::pixel) != 0) out += out.empty() ? "ps" : "+ps";
  if ((stages & shader_stage::compute) != 0) out += out.empty() ? "cs" : "+cs";
  return out.empty() ? "other" : out;
}

}  // namespace

void Descriptors::RegisterEvents() {
  reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
  reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
  reshade::register_event<reshade::addon_event::init_pipeline_layout>(OnInitPipelineLayout);
  reshade::register_event<reshade::addon_event::destroy_pipeline_layout>(OnDestroyPipelineLayout);
  reshade::register_event<reshade::addon_event::copy_descriptor_tables>(OnCopyDescriptorTables);
  reshade::register_event<reshade::addon_event::update_descriptor_tables>(OnUpdateDescriptorTables);
}

void Descriptors::UnregisterEvents() {
  reshade::unregister_event<reshade::addon_event::init_device>(OnInitDevice);
  reshade::unregister_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
  reshade::unregister_event<reshade::addon_event::init_pipeline_layout>(OnInitPipelineLayout);
  reshade::unregister_event<reshade::addon_event::destroy_pipeline_layout>(OnDestroyPipelineLayout);
  reshade::unregister_event<reshade::addon_event::copy_descriptor_tables>(OnCopyDescriptorTables);
  reshade::unregister_event<reshade::addon_event::update_descriptor_tables>(OnUpdateDescriptorTables);
}

bool Descriptors::LocateShaderResource(pipeline_layout layout, const std::vector<descriptor_table>& tables,
                                       shader_stage stage, uint32_t reg, uint32_t space, Slot* slot) const {
  const auto it = layouts_.find(layout.handle);
  if (it == layouts_.end()) return false;
  const std::vector<Param>& params = it->second;
  for (size_t index = 0; index < params.size() && index < tables.size(); ++index) {
    if (tables[index] == 0 || !IsTable(params[index].type)) continue;
    for (const descriptor_range& range : params[index].ranges) {
      if ((range.visibility & stage) == 0 || !IsShaderResource(range.type) || range.dx_register_space != space) continue;
      if (reg < range.dx_register_index || (range.count != UINT32_MAX && reg - range.dx_register_index >= range.count))
        continue;
      slot->param = static_cast<uint32_t>(index);
      slot->binding = range.binding + (reg - range.dx_register_index);
      return true;
    }
  }
  return false;
}

bool Descriptors::LocateRootConstantBuffer(pipeline_layout layout, shader_stage stage, uint32_t reg, uint32_t space,
                                           uint32_t* param) const {
  const auto it = layouts_.find(layout.handle);
  if (it == layouts_.end()) return false;
  for (size_t index = 0; index < it->second.size(); ++index) {
    const Param& p = it->second[index];
    if (!IsPush(p.type) || p.ranges.size() != 1) continue;
    const descriptor_range& r = p.ranges[0];
    if (r.type == descriptor_type::constant_buffer && (r.visibility & stage) != 0 && r.dx_register_index == reg &&
        r.dx_register_space == space) {
      *param = static_cast<uint32_t>(index);
      return true;
    }
  }
  return false;
}

std::string Descriptors::Describe(pipeline_layout layout) const {
  const auto it = layouts_.find(layout.handle);
  if (it == layouts_.end()) return "unknown layout";
  std::string out = std::to_string(it->second.size()) + " parameters:";
  for (size_t index = 0; index < it->second.size(); ++index) {
    const Param& p = it->second[index];
    std::string kind = "type" + std::to_string(static_cast<int>(p.type));
    if (IsTable(p.type)) kind = "table";
    if (IsPush(p.type)) kind = "push";
    if (p.type == pipeline_layout_param_type::push_constants) kind = "constants";
    out += " " + std::to_string(index) + ":" + kind + "(";
    for (size_t r = 0; r < p.ranges.size(); ++r) {
      const descriptor_range& range = p.ranges[r];
      out += (r == 0 ? "" : ", ") + std::string(TypeName(range.type)) + " r" + std::to_string(range.dx_register_index) +
             (range.dx_register_space != 0 ? " space" + std::to_string(range.dx_register_space) : "") +
             (range.count != 1 ? " x" + std::to_string(static_cast<int>(range.count)) : "") + " " +
             Stages(range.visibility);
    }
    out += ")";
  }
  return out;
}

resource_view Descriptors::View(descriptor_heap heap, uint32_t offset) const {
  const auto it = heaps_.find(heap.handle);
  if (it == heaps_.end() || offset >= it->second.descriptors.size()) return {0};
  const Descriptor& d = it->second.descriptors[offset];
  return IsShaderResource(d.type) ? d.view : resource_view{0};
}

void Descriptors::OnInitDevice(device* device) { device->create_private_data<Descriptors>(); }

void Descriptors::OnDestroyDevice(device* device) { device->destroy_private_data<Descriptors>(); }

void Descriptors::OnInitPipelineLayout(device* device, uint32_t count, const pipeline_layout_param* params,
                                       pipeline_layout layout) {
  auto* self = device->get_private_data<Descriptors>();
  if (self == nullptr) return;
  std::vector<Param> copy(count);
  for (uint32_t i = 0; i < count; ++i) {
    copy[i].type = params[i].type;
    switch (params[i].type) {
      case pipeline_layout_param_type::descriptor_table:
      case pipeline_layout_param_type::push_descriptors_with_ranges:
        copy[i].ranges.assign(params[i].descriptor_table.ranges,
                              params[i].descriptor_table.ranges + params[i].descriptor_table.count);
        break;
      case pipeline_layout_param_type::descriptor_table_with_flags:
      case pipeline_layout_param_type::push_descriptors_with_ranges_and_flags:
        for (uint32_t r = 0; r < params[i].descriptor_table_with_flags.count; ++r)
          copy[i].ranges.push_back(params[i].descriptor_table_with_flags.ranges[r]);  // slices off the flags
        break;
      case pipeline_layout_param_type::push_descriptors:
        copy[i].ranges.push_back(params[i].push_descriptors);
        break;
      default:
        break;
    }
  }
  self->layouts_[layout.handle] = std::move(copy);
}

void Descriptors::OnDestroyPipelineLayout(device* device, pipeline_layout layout) {
  auto* self = device->get_private_data<Descriptors>();
  if (self == nullptr) return;
  const auto it = self->layouts_.find(layout.handle);
  if (it != self->layouts_.end()) it->second.clear();  // concurrent_unordered_map cannot erase safely
}

bool Descriptors::OnCopyDescriptorTables(device* device, uint32_t count, const descriptor_table_copy* copies) {
  auto* self = device->get_private_data<Descriptors>();
  if (self == nullptr) return false;
  for (uint32_t i = 0; i < count; ++i) {
    const descriptor_table_copy& copy = copies[i];
    descriptor_heap src_heap, dst_heap;
    uint32_t src_offset = 0, dst_offset = 0;
    device->get_descriptor_heap_offset(copy.source_table, copy.source_binding, copy.source_array_offset, &src_heap,
                                       &src_offset);
    device->get_descriptor_heap_offset(copy.dest_table, copy.dest_binding, copy.dest_array_offset, &dst_heap,
                                       &dst_offset);
    HeapData& src = self->heaps_[src_heap.handle];
    HeapData& dst = self->heaps_[dst_heap.handle];
    if (src_offset + copy.count > src.descriptors.size()) src.descriptors.grow_to_at_least(src_offset + copy.count);
    if (dst_offset + copy.count > dst.descriptors.size()) dst.descriptors.grow_to_at_least(dst_offset + copy.count);
    for (uint32_t k = 0; k < copy.count; ++k) dst.descriptors[dst_offset + k] = src.descriptors[src_offset + k];
  }
  return false;
}

bool Descriptors::OnUpdateDescriptorTables(device* device, uint32_t count, const descriptor_table_update* updates) {
  auto* self = device->get_private_data<Descriptors>();
  if (self == nullptr) return false;
  for (uint32_t i = 0; i < count; ++i) {
    const descriptor_table_update& update = updates[i];
    descriptor_heap heap;
    uint32_t offset = 0;
    device->get_descriptor_heap_offset(update.table, update.binding, update.array_offset, &heap, &offset);
    HeapData& data = self->heaps_[heap.handle];
    if (offset + update.count > data.descriptors.size()) data.descriptors.grow_to_at_least(offset + update.count);
    for (uint32_t k = 0; k < update.count; ++k) {
      Descriptor& d = data.descriptors[offset + k];
      d.type = update.type;
      d.view = IsShaderResource(update.type) ? static_cast<const resource_view*>(update.descriptors)[k]
                                             : resource_view{0};
    }
  }
  return false;
}

}  // namespace npcmotion
