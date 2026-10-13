// Tracks root signatures (pipeline layouts) and the contents of descriptor heaps, so the resource view bound to a
// shader register at a draw can be looked up and replaced. Adapted from ReShade's examples/utils/descriptor_tracking
// (Copyright (C) 2021 Patrick Mours, BSD-3-Clause OR MIT) by way of the Elden Ring DLSS add-on.
// SPDX-License-Identifier: MIT

#pragma once

#include <reshade.hpp>

#include <concurrent_unordered_map.h>
#include <concurrent_vector.h>

#include <cstdint>
#include <vector>

namespace npcmotion {

class __declspec(uuid("4c1e8a37-5f0b-4d2e-9a6c-7b3d2e8f1a54")) Descriptors {
 public:
  // Registers the add-on events the tracking needs. Call once from DllMain after register_addon.
  static void RegisterEvents();
  static void UnregisterEvents();

  // Where the shader resource view of register t`reg` (space `space`) for `stage` lives, given the layout and the
  // descriptor tables bound per root parameter (index = parameter): the table's parameter and the binding (offset
  // in descriptors from the table's start). False when no bound table covers the register.
  struct Slot {
    uint32_t param = 0;
    uint32_t binding = 0;
  };
  bool LocateShaderResource(reshade::api::pipeline_layout layout,
                            const std::vector<reshade::api::descriptor_table>& tables,
                            reshade::api::shader_stage stage, uint32_t reg, uint32_t space, Slot* slot) const;

  // The root parameter holding constant buffer b`reg` (space `space`) for `stage` as a root descriptor.
  bool LocateRootConstantBuffer(reshade::api::pipeline_layout layout, reshade::api::shader_stage stage, uint32_t reg,
                                uint32_t space, uint32_t* param) const;

  // The resource view last written to a heap slot (by the game's view creation, descriptor copies or updates).
  reshade::api::resource_view View(reshade::api::descriptor_heap heap, uint32_t offset) const;

 private:
  struct Param {
    reshade::api::pipeline_layout_param_type type = reshade::api::pipeline_layout_param_type::push_descriptors;
    std::vector<reshade::api::descriptor_range> ranges;
  };
  struct Descriptor {
    reshade::api::descriptor_type type = reshade::api::descriptor_type::sampler;
    reshade::api::resource_view view = {0};
  };
  struct HeapData {
    concurrency::concurrent_vector<Descriptor> descriptors;
  };
  struct HandleHash {
    size_t operator()(uint64_t handle) const { return std::hash<uint64_t>()(handle); }
  };

  static void OnInitDevice(reshade::api::device* device);
  static void OnDestroyDevice(reshade::api::device* device);
  static void OnInitPipelineLayout(reshade::api::device* device, uint32_t count,
                                   const reshade::api::pipeline_layout_param* params,
                                   reshade::api::pipeline_layout layout);
  static void OnDestroyPipelineLayout(reshade::api::device* device, reshade::api::pipeline_layout layout);
  static bool OnCopyDescriptorTables(reshade::api::device* device, uint32_t count,
                                     const reshade::api::descriptor_table_copy* copies);
  static bool OnUpdateDescriptorTables(reshade::api::device* device, uint32_t count,
                                       const reshade::api::descriptor_table_update* updates);

  concurrency::concurrent_unordered_map<uint64_t, HeapData, HandleHash> heaps_;
  concurrency::concurrent_unordered_map<uint64_t, std::vector<Param>, HandleHash> layouts_;
};

}  // namespace npcmotion
