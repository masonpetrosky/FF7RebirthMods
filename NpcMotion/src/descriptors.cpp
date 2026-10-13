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
        copy[i].ranges.assign(params[i].descriptor_table.ranges,
                              params[i].descriptor_table.ranges + params[i].descriptor_table.count);
        break;
      case pipeline_layout_param_type::descriptor_table_with_flags:
        for (uint32_t r = 0; r < params[i].descriptor_table_with_flags.count; ++r)
          copy[i].ranges.push_back(params[i].descriptor_table_with_flags.ranges[r]);  // slices off the flags
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
