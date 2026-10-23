// SPDX-License-Identifier: MIT

#include "descriptors.hpp"

using namespace reshade::api;

namespace p3rdlss {
namespace {

bool Matches(RegisterKind kind, descriptor_type type) {
  switch (kind) {
    case RegisterKind::kConstantBuffer:
      return type == descriptor_type::constant_buffer;
    case RegisterKind::kShaderResource:
      return type == descriptor_type::shader_resource_view || type == descriptor_type::buffer_shader_resource_view ||
             type == descriptor_type::acceleration_structure;
    case RegisterKind::kUnorderedAccess:
      return type == descriptor_type::unordered_access_view || type == descriptor_type::buffer_unordered_access_view;
    case RegisterKind::kSampler:
      return type == descriptor_type::sampler || type == descriptor_type::sampler_with_resource_view;
  }
  return false;
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

bool Descriptors::Find(device* device, pipeline_layout layout, const std::vector<descriptor_table>& tables,
                       shader_stage stage, RegisterKind kind, uint32_t reg, uint32_t space, descriptor_heap* heap,
                       uint32_t* offset) const {
  const auto it = layouts_.find(layout.handle);
  if (it == layouts_.end()) return false;
  const std::vector<Param>& params = it->second;
  for (size_t index = 0; index < params.size() && index < tables.size(); ++index) {
    if (tables[index] == 0) continue;
    const Param& param = params[index];
    if (param.type != pipeline_layout_param_type::descriptor_table &&
        param.type != pipeline_layout_param_type::descriptor_table_with_flags)
      continue;
    for (const descriptor_range& range : param.ranges) {
      if ((range.visibility & stage) == 0 || !Matches(kind, range.type) || range.dx_register_space != space) continue;
      if (reg < range.dx_register_index || (range.count != UINT32_MAX && reg - range.dx_register_index >= range.count))
        continue;
      device->get_descriptor_heap_offset(tables[index], range.binding + (reg - range.dx_register_index), 0, heap,
                                         offset);
      return true;
    }
  }
  return false;
}

resource_view Descriptors::View(descriptor_heap heap, uint32_t offset) const {
  const auto it = heaps_.find(heap.handle);
  if (it == heaps_.end() || offset >= it->second.descriptors.size()) return {0};
  const Descriptor& d = it->second.descriptors[offset];
  return Matches(RegisterKind::kShaderResource, d.type) || Matches(RegisterKind::kUnorderedAccess, d.type) ||
                 d.type == descriptor_type::sampler_with_resource_view
             ? d.view
             : resource_view{0};
}

buffer_range Descriptors::Buffer(descriptor_heap heap, uint32_t offset) const {
  const auto it = heaps_.find(heap.handle);
  if (it == heaps_.end() || offset >= it->second.descriptors.size()) return {};
  const Descriptor& d = it->second.descriptors[offset];
  return d.type == descriptor_type::constant_buffer || d.type == descriptor_type::shader_storage_buffer
             ? d.buffer
             : buffer_range{};
}

std::vector<Descriptors::ParamInfo> Descriptors::Params(pipeline_layout layout) const {
  std::vector<ParamInfo> out;
  const auto it = layouts_.find(layout.handle);
  if (it == layouts_.end()) return out;
  for (const Param& param : it->second) out.push_back({param.type, param.ranges});
  return out;
}

std::string Descriptors::Describe(pipeline_layout layout) const {
  const auto it = layouts_.find(layout.handle);
  if (it == layouts_.end()) return "unknown layout\n";
  static const char* kTypes[] = {"table", "root constants", "root descriptor", "push ranges", "table", "push ranges"};
  static const char* kKinds[] = {"s", "s+view", "t", "u", "t(buf)", "u(buf)", "b", "st", "b(dyn)", "st(dyn)", "as"};
  std::string out;
  for (size_t i = 0; i < it->second.size(); ++i) {
    const Param& param = it->second[i];
    const auto type = static_cast<uint32_t>(param.type);
    out += "  [" + std::to_string(i) + "] " + (type < 6 ? kTypes[type] : "?") + ":";
    for (const descriptor_range& r : param.ranges) {
      if (param.type == pipeline_layout_param_type::push_constants) {
        out += " b" + std::to_string(r.dx_register_index) + " (" + std::to_string(r.count) + " dwords) vis" +
               std::to_string(static_cast<uint32_t>(r.visibility)) + ";";
        continue;
      }
      const auto kind = static_cast<uint32_t>(r.type);
      const std::string last =
          r.count == UINT32_MAX ? "+" : r.count > 1 ? "-" + std::to_string(r.dx_register_index + r.count - 1) : "";
      out += " " + std::string(kind < 11 ? kKinds[kind] : "?") + std::to_string(r.dx_register_index) + last +
             (r.dx_register_space != 0 ? " space" + std::to_string(r.dx_register_space) : "") + " @" +
             std::to_string(r.binding) + " vis" + std::to_string(static_cast<uint32_t>(r.visibility)) + ";";
    }
    out += "\n";
  }
  return out;
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
      case pipeline_layout_param_type::push_descriptors:
        copy[i].ranges.push_back(params[i].push_descriptors);
        break;
      case pipeline_layout_param_type::push_constants: {
        const constant_range& c = params[i].push_constants;
        copy[i].ranges.push_back({c.binding, c.dx_register_index, c.dx_register_space, c.count, c.visibility, 1,
                                  descriptor_type::constant_buffer});
        break;
      }
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
      switch (update.type) {
        case descriptor_type::sampler_with_resource_view:
          d.view = static_cast<const sampler_with_resource_view*>(update.descriptors)[k].view;
          break;
        case descriptor_type::shader_resource_view:
        case descriptor_type::unordered_access_view:
        case descriptor_type::buffer_shader_resource_view:
        case descriptor_type::buffer_unordered_access_view:
        case descriptor_type::acceleration_structure:
          d.view = static_cast<const resource_view*>(update.descriptors)[k];
          break;
        case descriptor_type::constant_buffer:
        case descriptor_type::shader_storage_buffer:
          d.buffer = static_cast<const buffer_range*>(update.descriptors)[k];
          break;
        default:
          break;
      }
    }
  }
  return false;
}

}  // namespace p3rdlss
