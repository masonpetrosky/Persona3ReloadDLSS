// Persona 3 Reload DLSS
// ReShade add-on that replaces Persona 3 Reload's temporal anti-aliasing, Unreal Engine 4.27's TAA compute pass, with
// NVIDIA DLSS. At the TAA dispatch it hands DLSS the pass's own scene color and depth, motion vectors made from the
// game's velocity buffer, the camera jitter and the eye adaptation exposure, writes the result into the pass's output
// (which bloom, the tonemapper and the next frame read) and skips the original dispatch. While DLSS runs, the
// engine's camera jitter gets more phases (r.TemporalAASamples). docs/recon.md has the pass's bindings and constants.
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "console_variables.hpp"
#include "descriptors.hpp"
#include "dlss.hpp"
#include "state_tracking.hpp"
#include "kMotionVectorsCs.h"

extern "C" __declspec(dllexport) const char* NAME = "Persona 3 Reload DLSS";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Replaces Persona 3 Reload's temporal anti-aliasing with NVIDIA DLSS (DLAA). Uses the NVIDIA DLSS SDK.";

using namespace reshade::api;
using p3rdlss::RegisterKind;

namespace {

namespace fs = std::filesystem;

// --- The game's TAA pass (docs/recon.md) -----------------------------------------------------------------------

// TAAStandalone.usf MainCS, by the digest in its DXIL container's HASH part.
constexpr uint8_t kTaaHash[16] = {0x20, 0x31, 0x52, 0x69, 0x4a, 0xc6, 0xf5, 0xd3,
                                  0xb9, 0x8e, 0x54, 0x64, 0x59, 0x8f, 0x56, 0x69};

// Its registers.
constexpr uint32_t kEyeAdaptationSrv = 0;  // 1x1 R32G32B32A32_FLOAT; x = the exposure the tonemapper applies
constexpr uint32_t kSceneColorSrv = 1;     // InputSceneColor, linear HDR
constexpr uint32_t kDepthSrv = 2;          // the scene depth (its depth plane), reverse Z
constexpr uint32_t kVelocitySrv = 3;       // GBufferVelocityTexture; a 1x1 dummy in the planar reflection's TAA
constexpr uint32_t kOutputUav = 0;         // the new history, which is the pass's output
constexpr uint32_t kGlobalsCbv = 0;        // $Globals, a root constant buffer
constexpr uint32_t kViewCbv = 1;           // View (FViewUniformShaderParameters), a root constant buffer

// Offsets in bytes.
constexpr uint32_t kCameraCut = 8;              // $Globals: int bCameraCut
constexpr uint32_t kClipToPrevClip = 1952;      // View: float4x4 ClipToPrevClip, rows, without jitter
constexpr uint32_t kTemporalAAJitter = 2016;    // View: float4, xy = this frame's jitter in clip units
constexpr uint32_t kViewRectMin = 2064;         // View: float4
constexpr uint32_t kViewSizeAndInvSize = 2080;  // View: float4
constexpr uint32_t kPreExposure = 2164;         // View: float, the exposure the scene color is stored with
constexpr uint32_t kViewBegin = kClipToPrevClip;  // the part of View the add-on reads
constexpr uint32_t kViewEnd = kPreExposure + 4;

// What the add-on reads from the TAA's constant buffers.
struct TaaConstants {
  bool camera_cut = false;
  float clip_to_prev_clip[16] = {};
  float jitter[2] = {};  // clip units
  float view_rect_min[2] = {};
  float view_size[4] = {};  // width, height, 1 / width, 1 / height
  float pre_exposure = 1.0f;
};

// motion_vectors.hlsl's root constants (b0).
struct MotionConstants {
  float clip_to_prev_clip[16];
  float view_size_and_inv_size[4];
  float view_rect_min[2];
  uint32_t mode;
  uint32_t unused;
};
static_assert(sizeof(MotionConstants) == 24 * 4);

struct __declspec(uuid("6d2f8a41-93c7-4e5b-a0d6-1f4b7c9e2a58")) CmdData {
  pipeline current = {0};  // the pipeline state the game set last
  bool taa = false;        // it is the TAA's
  // The root constant buffers the game set (SetComputeRootConstantBufferView) for the compute root signature
  // `cbv_layout`, by root parameter. The descriptor tracking only covers descriptor tables.
  pipeline_layout cbv_layout = {0};
  std::vector<buffer_range> cbvs;
};

// --- Settings (Persona3ReloadDLSS.ini next to the add-on) ------------------------------------------------------

struct Config {
  std::atomic<bool> enabled{true};
  int preset = 0;  // 0 = NGX's default, which is preset K for DLAA
  int toggle_key = VK_F9;
  // The engine's camera jitter phases (r.TemporalAASamples) while DLSS runs; 0 keeps the game's. The game sets 2 in
  // the places seen so far, too few for DLSS, which wants 8 or more.
  int jitter_phases = 16;
  bool game_exposure = true;  // give DLSS the game's eye adaptation instead of letting it estimate an exposure
  int motion_mode = 0;        // development: 0 the game's velocity and camera motion, 1 camera motion only, 2 zero
};

Config g_config;
fs::path g_addon_dir;  // the game's Binaries\Win64
fs::path g_data_dir;   // Persona3ReloadDLSS in it: nvngx_dlss.dll and NGX's logs
fs::path g_ini_path;

device* g_device = nullptr;
p3rdlss::Dlss g_dlss;
std::mutex g_dlss_mutex;  // g_dlss once NGX started, and the add-on's resources below

// NGX starts on a thread of its own once the game's swap chain exists (StartNvidia): Unreal first creates a device
// only to test the adapter and releases it again, which unloads the add-on. Until g_nvidia_ready, g_dlss belongs to
// that thread and the game keeps its TAA.
std::thread g_nvidia_thread;
std::atomic<bool> g_nvidia_started{false};
std::atomic<bool> g_nvidia_ready{false};
std::atomic<bool> g_dlss_available{false};
std::atomic<uint64_t> g_nvidia_start_ms{0};

// r.TemporalAASamples' values ([0] the game thread's, [1] the render thread's, which the jitter uses), found by the
// NVIDIA thread; g_jitter_detail says how, readable once g_nvidia_ready.
std::atomic<int32_t*> g_taa_samples{nullptr};
std::string g_jitter_detail;

std::atomic<uint64_t> g_frame{0};
std::atomic<float> g_frame_time_ms{0.0f};
std::atomic<bool> g_reset{true};
std::atomic<bool> g_recreate{false};  // release the DLSS feature at the next present, once the GPU is idle
std::atomic<uint64_t> g_failures{0};
constexpr uint64_t kMaxFailures = 5;  // failed evaluations (each recreates the feature) before DLSS turns itself off
std::atomic<uint64_t> g_taa_dispatches{0};  // the main view's TAA dispatches seen
std::atomic<uint64_t> g_dlss_frames{0};
std::atomic<uint64_t> g_last_dlss_frame{0};  // g_frame when DLSS last ran
std::atomic<uint32_t> g_out_width{0}, g_out_height{0};
std::atomic<float> g_last_jitter_x{0.0f}, g_last_jitter_y{0.0f};

std::shared_mutex g_taa_mutex;
std::unordered_set<uint64_t> g_taa_pipelines;

// The motion vector pass and its textures, under g_dlss_mutex.
resource g_motion = {0};  // R16G16_FLOAT, render pixels from this frame's position to the previous one
resource_view g_motion_uav = {0};
resource g_exposure = {0};  // 1x1 R32_FLOAT, the exposure DLSS reads
resource_view g_exposure_uav = {0};
pipeline_layout g_mv_layout = {0};
pipeline g_mv_pipeline = {0};
bool g_mv_failed = false;

// Textures replaced while the GPU may still use them, destroyed a few frames later.
struct Retired {
  resource res;
  resource_view view;
  uint64_t frame;
};
std::vector<Retired> g_retired;

// Shader resource views of the game's textures in the formats the motion vector pass reads, until the game destroys
// the texture.
std::mutex g_view_mutex;
std::unordered_map<uint64_t, resource_view> g_views;

std::mutex g_info_mutex;
std::string g_taa_info = "TAA pass not seen yet.";
std::string g_logged_info;

void Log(reshade::log::level level, const std::string& message) {
  reshade::log::message(level, ("[P3RDLSS] " + message).c_str());
}

int ReadInt(const wchar_t* key, int fallback) {
  return static_cast<int>(GetPrivateProfileIntW(L"Persona3ReloadDLSS", key, fallback, g_ini_path.c_str()));
}

void WriteInt(const wchar_t* key, int value) {
  WritePrivateProfileStringW(L"Persona3ReloadDLSS", key, std::to_wstring(value).c_str(), g_ini_path.c_str());
}

void LoadConfig() {
  g_config.enabled = ReadInt(L"Enabled", 1) != 0;
  g_config.preset = ReadInt(L"Preset", 0);
  g_config.toggle_key = ReadInt(L"ToggleKey", VK_F9);
  g_config.jitter_phases = std::clamp(ReadInt(L"JitterPhases", 16), 0, 64);
  g_config.game_exposure = ReadInt(L"GameExposure", 1) != 0;
  g_config.motion_mode = std::clamp(ReadInt(L"MotionVectors", 0), 0, 2);
}

void SaveConfig() {
  WriteInt(L"Enabled", g_config.enabled);
  WriteInt(L"Preset", g_config.preset);
  WriteInt(L"ToggleKey", g_config.toggle_key);
  WriteInt(L"JitterPhases", g_config.jitter_phases);
  WriteInt(L"GameExposure", g_config.game_exposure);
  WriteInt(L"MotionVectors", g_config.motion_mode);
}

void SetDlssEnabled(bool enabled) {
  g_config.enabled = enabled;
  g_reset = true;
}

std::string KeyName(int vk) {
  char text[16];
  if (vk >= VK_F1 && vk <= VK_F24)
    std::snprintf(text, sizeof(text), "F%d", vk - VK_F1 + 1);
  else
    std::snprintf(text, sizeof(text), "key 0x%02X", vk);
  return text;
}

// --- Pass detection --------------------------------------------------------------------------------------------

// A DXIL container: "DXBC", its digest, version, size, part count and part offsets; each part starts with its
// four-character code and size. The HASH part holds flags and the shader's digest.
bool IsTaaShader(const shader_desc* cs) {
  if (cs == nullptr || cs->code == nullptr || cs->code_size < 32) return false;
  const auto* const code = static_cast<const uint8_t*>(cs->code);
  if (std::memcmp(code, "DXBC", 4) != 0) return false;
  uint32_t parts = 0;
  std::memcpy(&parts, code + 28, 4);
  for (uint32_t i = 0; i < parts && 32 + 4 * (size_t{i} + 1) <= cs->code_size; ++i) {
    uint32_t offset = 0;
    std::memcpy(&offset, code + 32 + 4 * i, 4);
    if (offset > cs->code_size || cs->code_size - offset < 28) continue;
    if (std::memcmp(code + offset, "HASH", 4) == 0) return std::memcmp(code + offset + 12, kTaaHash, 16) == 0;
  }
  return false;
}

void OnInitPipeline(device*, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects, pipeline p) {
  for (uint32_t i = 0; i < count; ++i) {
    if (subobjects[i].type != pipeline_subobject_type::compute_shader) continue;
    if (!IsTaaShader(static_cast<const shader_desc*>(subobjects[i].data))) return;
    size_t total = 0;
    {
      std::unique_lock lock(g_taa_mutex);
      g_taa_pipelines.insert(p.handle);
      total = g_taa_pipelines.size();
    }
    char text[96];
    std::snprintf(text, sizeof(text), "Found the TAA shader (pipeline %llx, %zu so far).",
                  static_cast<unsigned long long>(p.handle), total);
    Log(reshade::log::level::info, text);
    return;
  }
}

void OnDestroyPipeline(device*, pipeline p) {
  std::unique_lock lock(g_taa_mutex);
  g_taa_pipelines.erase(p.handle);
}

void OnInitCommandList(command_list* cmd_list) { cmd_list->create_private_data<CmdData>(); }

void OnDestroyCommandList(command_list* cmd_list) { cmd_list->destroy_private_data<CmdData>(); }

void OnResetCommandList(command_list* cmd_list) {
  if (auto* data = cmd_list->get_private_data<CmdData>()) *data = CmdData();
}

void OnBindPipeline(command_list* cmd_list, pipeline_stage, pipeline p) {
  auto* const data = cmd_list->get_private_data<CmdData>();
  if (data == nullptr) return;
  data->current = p;
  std::shared_lock lock(g_taa_mutex);
  data->taa = g_taa_pipelines.count(p.handle) != 0;
}

void OnPushDescriptors(command_list* cmd_list, shader_stage stages, pipeline_layout layout, uint32_t param,
                       const descriptor_table_update& update) {
  if ((stages & shader_stage::compute) == 0 || update.type != descriptor_type::constant_buffer || update.count != 1 ||
      update.descriptors == nullptr)
    return;
  auto* const data = cmd_list->get_private_data<CmdData>();
  if (data == nullptr) return;
  if (data->cbv_layout != layout) {  // a new root signature resets the root arguments
    data->cbv_layout = layout;
    data->cbvs.clear();
  }
  if (param >= data->cbvs.size()) data->cbvs.resize(param + 1);
  data->cbvs[param] = static_cast<const buffer_range*>(update.descriptors)[0];
}

// --- Binding lookups -------------------------------------------------------------------------------------------

using Tables = std::pair<pipeline_layout, std::vector<descriptor_table>>;

// The compute root signature and the descriptor tables bound with it.
const Tables* ComputeTables(const state_tracking& state) {
  for (const auto& [stages, tables] : state.descriptor_tables)
    if ((stages & shader_stage::compute) != 0) return &tables;
  return nullptr;
}

resource BoundResource(device* dev, const p3rdlss::Descriptors& descriptors, const Tables& tables, RegisterKind kind,
                       uint32_t reg) {
  descriptor_heap heap = {0};
  uint32_t offset = 0;
  if (!descriptors.Find(dev, tables.first, tables.second, shader_stage::compute, kind, reg, 0, &heap, &offset))
    return {0};
  const resource_view view = descriptors.View(heap, offset);
  return view == 0 ? resource{0} : dev->get_resource_from_view(view);
}

// Copies `size` bytes at `offset` in a constant buffer the game bound (Unreal keeps them in upload heaps).
bool ReadBuffer(device* dev, const buffer_range& range, uint32_t offset, uint32_t size, void* out) {
  if (range.buffer == 0) return false;
  const resource_desc desc = dev->get_resource_desc(range.buffer);
  if (desc.type != resource_type::buffer || desc.heap != memory_heap::upload ||
      range.offset + offset + size > desc.buffer.size)
    return false;
  void* mapped = nullptr;
  if (!dev->map_buffer_region(range.buffer, range.offset + offset, size, map_access::read_only, &mapped) ||
      mapped == nullptr)
    return false;
  std::memcpy(out, mapped, size);
  dev->unmap_buffer_region(range.buffer);
  return true;
}

bool ReadTaaConstants(device* dev, const p3rdlss::Descriptors& descriptors, const CmdData& data,
                      pipeline_layout layout, TaaConstants* out, std::string* why) {
  uint32_t globals = UINT32_MAX, view = UINT32_MAX;
  const std::vector<p3rdlss::Descriptors::ParamInfo> params = descriptors.Params(layout);
  for (uint32_t i = 0; i < params.size(); ++i) {
    // A root CBV: push_descriptors in root signature 1.0, push_descriptors_with_ranges_and_flags in 1.1 (Unreal's).
    const p3rdlss::Descriptors::ParamInfo& p = params[i];
    if ((p.type != pipeline_layout_param_type::push_descriptors &&
         p.type != pipeline_layout_param_type::push_descriptors_with_ranges &&
         p.type != pipeline_layout_param_type::push_descriptors_with_ranges_and_flags) ||
        p.ranges.size() != 1 || p.ranges[0].type != descriptor_type::constant_buffer ||
        p.ranges[0].dx_register_space != 0)
      continue;
    if (p.ranges[0].dx_register_index == kGlobalsCbv) globals = i;
    if (p.ranges[0].dx_register_index == kViewCbv) view = i;
  }
  if (globals == UINT32_MAX || view == UINT32_MAX) {
    *why = "its root signature has no root constant buffers b0 and b1";
    return false;
  }
  if (data.cbv_layout != layout || globals >= data.cbvs.size() || view >= data.cbvs.size() ||
      data.cbvs[globals].buffer == 0 || data.cbvs[view].buffer == 0) {
    size_t seen = 0;
    for (const buffer_range& range : data.cbvs) seen += range.buffer != 0;
    *why = "its constant buffers b0 and b1 (root parameters " + std::to_string(globals) + " and " +
           std::to_string(view) + ") were not seen; " + std::to_string(seen) + " root constant buffer(s) set" +
           (data.cbv_layout == layout ? "" : " for another root signature");
    return false;
  }
  int32_t camera_cut = 0;
  float v[(kViewEnd - kViewBegin) / 4];
  if (!ReadBuffer(dev, data.cbvs[globals], kCameraCut, sizeof(camera_cut), &camera_cut) ||
      !ReadBuffer(dev, data.cbvs[view], kViewBegin, sizeof(v), v)) {
    *why = "its constant buffers could not be read (heap type " +
           std::to_string(static_cast<int>(dev->get_resource_desc(data.cbvs[view].buffer).heap)) + ")";
    return false;
  }
  const auto at = [&v](uint32_t offset) { return v + (offset - kViewBegin) / 4; };
  out->camera_cut = camera_cut != 0;
  std::memcpy(out->clip_to_prev_clip, at(kClipToPrevClip), sizeof(out->clip_to_prev_clip));
  std::memcpy(out->jitter, at(kTemporalAAJitter), sizeof(out->jitter));
  std::memcpy(out->view_rect_min, at(kViewRectMin), sizeof(out->view_rect_min));
  std::memcpy(out->view_size, at(kViewSizeAndInvSize), sizeof(out->view_size));
  out->pre_exposure = *at(kPreExposure);
  return true;
}

// The format that reads a depth texture's depth plane, or unknown.
format DepthViewFormat(format f) {
  switch (format_to_typeless(f)) {
    case format::r32_g8_typeless: return format::r32_float_x8_uint;
    case format::r24_g8_typeless: return format::r24_unorm_x8_uint;
    case format::r32_typeless: return format::r32_float;
    case format::r16_typeless: return format::r16_unorm;
    default: return format::unknown;
  }
}

bool IsTexture(device* dev, resource res, uint32_t width, uint32_t height, format typeless) {
  if (res == 0) return false;
  const resource_desc d = dev->get_resource_desc(res);
  return d.type == resource_type::texture_2d && d.texture.width == width && d.texture.height == height &&
         (typeless == format::unknown || format_to_typeless(d.texture.format) == typeless);
}

const char* FormatName(format f) {
  switch (f) {
    case format::r16g16b16a16_float: return "R16G16B16A16_FLOAT";
    case format::r16g16b16a16_unorm: return "R16G16B16A16_UNORM";
    case format::r16g16b16a16_typeless: return "R16G16B16A16_TYPELESS";
    case format::r32g32b32a32_float: return "R32G32B32A32_FLOAT";
    case format::r32g32b32a32_typeless: return "R32G32B32A32_TYPELESS";
    case format::r32_g8_typeless: return "R32G8X24_TYPELESS";
    case format::d32_float_s8_uint: return "D32_FLOAT_S8X24_UINT";
    case format::r32_typeless: return "R32_TYPELESS";
    case format::r11g11b10_float: return "R11G11B10_FLOAT";
    case format::b8g8r8a8_unorm: return "B8G8R8A8_UNORM";
    default: return "other";
  }
}

std::string Describe(device* dev, resource res) {
  if (res == 0) return "none";
  const resource_desc d = dev->get_resource_desc(res);
  char text[96];
  std::snprintf(text, sizeof(text), "%ux%u %s (%d)", d.texture.width, d.texture.height, FormatName(d.texture.format),
                static_cast<int>(d.texture.format));
  return text;
}

// The overlay's line about the pass; logged whenever it changes.
void SetInfo(const std::string& info) {
  std::lock_guard lock(g_info_mutex);
  g_taa_info = info;
  if (info == g_logged_info) return;
  g_logged_info = info;
  Log(reshade::log::level::info, info);
}

// --- DLSS ------------------------------------------------------------------------------------------------------

void Retire(resource* res, resource_view* view) {
  if (*res != 0 || *view != 0) g_retired.push_back({*res, *view, g_frame.load()});
  *res = {0};
  *view = {0};
}

bool CreateTexture(device* dev, uint32_t width, uint32_t height, format fmt, const char* name, resource* res,
                   resource_view* uav) {
  const resource_desc desc(width, height, 1, 1, fmt, 1, memory_heap::default_,
                           resource_usage::unordered_access | resource_usage::shader_resource);
  if (!dev->create_resource(desc, nullptr, resource_usage::shader_resource, res)) {
    *res = {0};
    return false;
  }
  if (!dev->create_resource_view(*res, resource_usage::unordered_access, resource_view_desc(fmt), uav)) {
    dev->destroy_resource(*res);
    *res = {0};
    *uav = {0};
    return false;
  }
  dev->set_resource_name(*res, name);
  return true;
}

// The motion vector pass and its textures for a render size.
bool EnsurePass(device* dev, uint32_t width, uint32_t height) {
  if (g_mv_failed) return false;
  if (g_mv_pipeline == 0) {
    const pipeline_layout_param params[] = {
        constant_range{0, 0, 0, sizeof(MotionConstants) / 4, shader_stage::compute},
        descriptor_range{0, 0, 0, 3, shader_stage::compute, 1, descriptor_type::texture_shader_resource_view},
        descriptor_range{0, 0, 0, 2, shader_stage::compute, 1, descriptor_type::texture_unordered_access_view},
    };
    shader_desc cs = {};
    cs.code = kMotionVectorsCs;
    cs.code_size = sizeof(kMotionVectorsCs);
    const pipeline_subobject subobject = {pipeline_subobject_type::compute_shader, 1, &cs};
    if (!dev->create_pipeline_layout(3, params, &g_mv_layout) ||
        !dev->create_pipeline(g_mv_layout, 1, &subobject, &g_mv_pipeline)) {
      Log(reshade::log::level::error, "Could not create the motion vector shader; DLSS stays off.");
      g_mv_pipeline = {0};
      g_mv_failed = true;
      return false;
    }
  }
  if (g_motion != 0) {
    const resource_desc d = dev->get_resource_desc(g_motion);
    if (d.texture.width != width || d.texture.height != height) Retire(&g_motion, &g_motion_uav);
  }
  if ((g_motion == 0 && !CreateTexture(dev, width, height, format::r16g16_float, "Persona3ReloadDLSS motion vectors",
                                       &g_motion, &g_motion_uav)) ||
      (g_exposure == 0 &&
       !CreateTexture(dev, 1, 1, format::r32_float, "Persona3ReloadDLSS exposure", &g_exposure, &g_exposure_uav))) {
    Log(reshade::log::level::error, "Could not create the motion vector textures; DLSS stays off.");
    g_mv_failed = true;
    return false;
  }
  return true;
}

// A shader resource view of one of the game's textures.
resource_view GameView(device* dev, resource res, format fmt) {
  std::lock_guard lock(g_view_mutex);
  resource_view& view = g_views[res.handle];
  if (view == 0 && !dev->create_resource_view(res, resource_usage::shader_resource, resource_view_desc(fmt), &view)) {
    g_views.erase(res.handle);
    return {0};
  }
  return view;
}

void OnDestroyResource(device* dev, resource res) {
  std::lock_guard lock(g_view_mutex);
  const auto it = g_views.find(res.handle);
  if (it == g_views.end()) return;
  dev->destroy_resource_view(it->second);
  g_views.erase(it);
}

ID3D12DescriptorHeap* HeapOf(device* dev, descriptor_table table) {
  descriptor_heap heap = {0};
  dev->get_descriptor_heap_offset(table, 0, 0, &heap, nullptr);
  return reinterpret_cast<ID3D12DescriptorHeap*>(heap.handle);
}

// NGX and the motion vector pass bind descriptor heaps, a compute root signature, root arguments and a pipeline state
// of their own. Put the game's back: the heaps and the compute root signature directly (ReShade does not see what NGX
// binds), then through ReShade, which then tracks them again, every descriptor table the game has bound, the TAA's
// root constant buffers and its pipeline state.
void RestoreState(command_list* cmd_list, const CmdData& data) {
  auto* const state = cmd_list->get_private_data<state_tracking>();
  if (state == nullptr) return;
  device* const dev = cmd_list->get_device();
  auto* const native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());

  // The heaps of the TAA's own tables first: they are the ones bound now.
  ID3D12DescriptorHeap* heaps[2] = {};  // CBV/SRV/UAV, sampler
  const auto slot = [](ID3D12DescriptorHeap* h) {
    return h->GetDesc().Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 1 : 0;
  };
  for (const bool compute : {true, false}) {
    for (const auto& [stages, tables] : state->descriptor_tables) {
      if (((stages & shader_stage::compute) != 0) != compute) continue;
      for (const descriptor_table table : tables.second) {
        if (table == 0) continue;
        ID3D12DescriptorHeap* const h = HeapOf(dev, table);
        if (h != nullptr && heaps[slot(h)] == nullptr) heaps[slot(h)] = h;
      }
    }
  }
  ID3D12DescriptorHeap* bound[2];
  UINT count = 0;
  for (ID3D12DescriptorHeap* h : heaps)
    if (h != nullptr) bound[count++] = h;
  if (count != 0) native->SetDescriptorHeaps(count, bound);

  pipeline_layout compute_layout = {0};
  for (const auto& [stages, tables] : state->descriptor_tables) {
    if (tables.first == 0) continue;
    if ((stages & shader_stage::compute) != 0) {
      compute_layout = tables.first;
      native->SetComputeRootSignature(reinterpret_cast<ID3D12RootSignature*>(tables.first.handle));
    }
    for (uint32_t i = 0; i < tables.second.size(); ++i) {
      const descriptor_table table = tables.second[i];
      if (table == 0) continue;
      ID3D12DescriptorHeap* const h = HeapOf(dev, table);
      if (h == nullptr || heaps[slot(h)] != h) continue;  // from a heap the game has since replaced
      cmd_list->bind_descriptor_tables(stages, tables.first, i, 1, &table);
    }
  }
  if (compute_layout != 0 && data.cbv_layout == compute_layout) {
    for (uint32_t i = 0; i < data.cbvs.size(); ++i)
      if (data.cbvs[i].buffer != 0)
        cmd_list->push_descriptors(shader_stage::all_compute, compute_layout, i,
                                   descriptor_table_update{{}, 0, 0, 1, descriptor_type::constant_buffer,
                                                           &data.cbvs[i]});
  }
  if (data.current != 0) cmd_list->bind_pipeline(pipeline_stage::all, data.current);
}

// Records DLSS in place of the TAA dispatch. False keeps the game's dispatch.
bool ReplaceTaa(command_list* cmd_list, CmdData& data) {
  device* const dev = cmd_list->get_device();
  auto* const state = cmd_list->get_private_data<state_tracking>();
  auto* const descriptors = dev->get_private_data<p3rdlss::Descriptors>();
  if (state == nullptr || descriptors == nullptr) return false;
  const Tables* const tables = ComputeTables(*state);
  if (tables == nullptr || tables->first == 0) return false;
  const auto bound = [&](RegisterKind kind, uint32_t reg) {
    return BoundResource(dev, *descriptors, *tables, kind, reg);
  };
  const resource velocity = bound(RegisterKind::kShaderResource, kVelocitySrv);
  if (velocity == 0 || dev->get_resource_desc(velocity).texture.width <= 1) return false;  // the planar reflection's

  const uint64_t dispatch = ++g_taa_dispatches;
  const resource eye = bound(RegisterKind::kShaderResource, kEyeAdaptationSrv);
  const resource color = bound(RegisterKind::kShaderResource, kSceneColorSrv);
  const resource depth = bound(RegisterKind::kShaderResource, kDepthSrv);
  const resource output = bound(RegisterKind::kUnorderedAccess, kOutputUav);
  const resource_desc od = output != 0 ? dev->get_resource_desc(output) : resource_desc();
  const uint32_t width = od.texture.width, height = od.texture.height;
  const format depth_format = depth != 0 ? DepthViewFormat(dev->get_resource_desc(depth).texture.format) : format::unknown;
  g_out_width = width;
  g_out_height = height;

  TaaConstants c;
  std::string why;
  bool valid = ReadTaaConstants(dev, *descriptors, data, tables->first, &c, &why);
  const float jitter_x = c.jitter[0] * 0.5f * static_cast<float>(width);  // render pixels, x right and y down
  const float jitter_y = -c.jitter[1] * 0.5f * static_cast<float>(height);
  if (valid && !(IsTexture(dev, output, width, height, format::r16g16b16a16_typeless) &&
                 IsTexture(dev, color, width, height, format::r16g16b16a16_typeless) &&
                 IsTexture(dev, velocity, width, height, format::r16g16b16a16_typeless) &&
                 IsTexture(dev, depth, width, height, format::unknown) && depth_format != format::unknown &&
                 IsTexture(dev, eye, 1, 1, format::r32g32b32a32_typeless))) {
    valid = false;
    why = "unexpected textures";
  }
  if (valid && !(c.view_size[0] == static_cast<float>(width) && c.view_size[1] == static_cast<float>(height) &&
                 c.view_rect_min[0] == 0.0f && c.view_rect_min[1] == 0.0f)) {
    valid = false;
    why = "the view does not fill the output (a render scale below 100%?)";
  }
  if (valid && !(std::fabs(jitter_x) <= 1.0f && std::fabs(jitter_y) <= 1.0f && c.pre_exposure > 0.0f &&
                 std::isfinite(c.pre_exposure))) {
    valid = false;
    why = "unexpected constants";
  }
  {
    char view[64];
    std::snprintf(view, sizeof(view), "%.0fx%.0f at (%.0f, %.0f)", c.view_size[0], c.view_size[1],
                  c.view_rect_min[0], c.view_rect_min[1]);
    SetInfo("TAA: color " + Describe(dev, color) + ", depth " + Describe(dev, depth) + ", velocity " +
            Describe(dev, velocity) + ", eye adaptation " + Describe(dev, eye) + ", output " + Describe(dev, output) +
            ", view " + view + (valid ? "" : "; keeping the game's TAA: " + why));
  }
  if (!g_config.enabled || !valid || !g_nvidia_ready) return false;

  std::lock_guard lock(g_dlss_mutex);
  if (!g_dlss.available() || g_recreate.load() || !EnsurePass(dev, width, height)) return false;
  auto* const native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
  const bool game_exposure = g_config.game_exposure;
  if (g_dlss.has_feature() &&
      (g_dlss.feature_width() != width || g_dlss.feature_height() != height ||
       g_dlss.feature_preset() != g_config.preset || g_dlss.feature_auto_exposure() == game_exposure)) {
    g_recreate = true;  // the old feature may still be in flight; it is released at the present once the GPU is idle
    return false;
  }
  if (!g_dlss.has_feature()) {
    const bool created =
        g_dlss.Create(native, width, height, width, height, 1.0f, g_config.preset, true, !game_exposure);
    RestoreState(cmd_list, data);
    if (created) {
      Log(reshade::log::level::info, "Created the DLSS feature: " + g_dlss.status());
    } else {
      g_config.enabled = false;
      Log(reshade::log::level::error, g_dlss.status() + "; DLSS off until the next start.");
    }
    g_reset = true;
    return false;  // DLSS takes over from the next frame
  }

  const resource_view srvs[3] = {GameView(dev, velocity, format::r16g16b16a16_unorm), GameView(dev, depth, depth_format),
                                 GameView(dev, eye, format::r32g32b32a32_float)};
  if (srvs[0] == 0 || srvs[1] == 0 || srvs[2] == 0) return false;
  MotionConstants constants = {};
  std::memcpy(constants.clip_to_prev_clip, c.clip_to_prev_clip, sizeof(constants.clip_to_prev_clip));
  std::memcpy(constants.view_size_and_inv_size, c.view_size, sizeof(constants.view_size_and_inv_size));
  constants.mode = static_cast<uint32_t>(g_config.motion_mode);
  const resource_view uavs[2] = {g_motion_uav, g_exposure_uav};
  const resource ours[2] = {g_motion, g_exposure};
  const resource_usage readable[2] = {resource_usage::shader_resource, resource_usage::shader_resource};
  const resource_usage writable[2] = {resource_usage::unordered_access, resource_usage::unordered_access};
  cmd_list->barrier(2, ours, readable, writable);
  cmd_list->bind_pipeline(pipeline_stage::all_compute, g_mv_pipeline);
  cmd_list->push_constants(shader_stage::compute, g_mv_layout, 0, 0, sizeof(constants) / 4, &constants);
  cmd_list->push_descriptors(shader_stage::compute, g_mv_layout, 1,
                             descriptor_table_update{{}, 0, 0, 3, descriptor_type::texture_shader_resource_view, srvs});
  cmd_list->push_descriptors(shader_stage::compute, g_mv_layout, 2,
                             descriptor_table_update{{}, 0, 0, 2, descriptor_type::texture_unordered_access_view, uavs});
  cmd_list->dispatch((width + 7) / 8, (height + 7) / 8, 1);
  cmd_list->barrier(2, ours, writable, readable);

  const uint64_t frame = g_frame.load();
  p3rdlss::DlssInputs in;
  in.color = reinterpret_cast<ID3D12Resource*>(color.handle);
  in.depth = reinterpret_cast<ID3D12Resource*>(depth.handle);
  in.motion = reinterpret_cast<ID3D12Resource*>(g_motion.handle);
  in.output = reinterpret_cast<ID3D12Resource*>(output.handle);
  in.exposure = game_exposure ? reinterpret_cast<ID3D12Resource*>(g_exposure.handle) : nullptr;
  in.width = width;
  in.height = height;
  in.jitter_x = jitter_x;
  in.jitter_y = jitter_y;
  in.pre_exposure = c.pre_exposure;
  // A camera cut, a toggle, or frames without DLSS since it last ran (its history is stale then).
  const bool resumed = g_last_dlss_frame.load() + 2 < frame;
  in.reset = g_reset.exchange(false) || c.camera_cut || resumed;
  in.frame_time_ms = g_frame_time_ms.load();
  const bool ok = g_dlss.Evaluate(native, in);
  RestoreState(cmd_list, data);
  if (!ok) {
    g_reset = true;
    g_recreate = true;
    const uint64_t failures = ++g_failures;
    if (failures == 1) Log(reshade::log::level::error, g_dlss.status());
    if (failures == kMaxFailures) {
      g_config.enabled = false;
      Log(reshade::log::level::error, "DLSS failed " + std::to_string(failures) + " times; off until the next start.");
    }
    return false;
  }
  g_last_dlss_frame = frame;
  g_last_jitter_x = jitter_x;
  g_last_jitter_y = jitter_y;
  const uint64_t frames = ++g_dlss_frames;
  if (frames == 1 || frames % 3000 == 0) {
    const int32_t* const samples = g_taa_samples.load();
    char text[256];
    std::snprintf(text, sizeof(text),
                  "DLSS frame %llu (TAA dispatch %llu): %ux%u, jitter %+.3f %+.3f px, %d jitter phases, pre-exposure "
                  "%.3g, camera cut %d, reset %d, failures %llu",
                  static_cast<unsigned long long>(frames), static_cast<unsigned long long>(dispatch), width, height,
                  jitter_x, jitter_y, samples != nullptr ? samples[1] : -1, c.pre_exposure, c.camera_cut ? 1 : 0,
                  in.reset ? 1 : 0, static_cast<unsigned long long>(g_failures.load()));
    Log(reshade::log::level::info, text);
  }
  return true;
}

bool OnDispatch(command_list* cmd_list, uint32_t, uint32_t, uint32_t) {
  auto* const data = cmd_list->get_private_data<CmdData>();
  if (data == nullptr || !data->taa) return false;
  return ReplaceTaa(cmd_list, *data);
}

// --- Device and frame ------------------------------------------------------------------------------------------

void StartNvidia(ID3D12Device* device) {
  int32_t* samples = p3rdlss::FindIntConsoleVariable(L"r.TemporalAASamples", &g_jitter_detail);
  // Both copies hold a sample count (the engine's default is 8; the game sets 2 per area later).
  if (samples != nullptr && !(samples[0] >= 1 && samples[0] <= 64 && samples[1] >= 1 && samples[1] <= 64)) {
    g_jitter_detail += ", but its values " + std::to_string(samples[0]) + " and " + std::to_string(samples[1]) +
                       " are no sample counts";
    samples = nullptr;
  }
  if (samples != nullptr) {
    g_taa_samples = samples;
    Log(reshade::log::level::info, "r.TemporalAASamples " + g_jitter_detail + "; the game's value is " +
                                       std::to_string(samples[0]) + ".");
  } else {
    Log(reshade::log::level::warning,
        "r.TemporalAASamples not found (" + g_jitter_detail + "); the camera jitter keeps the game's phases.");
  }
  const fs::path log_dir = g_data_dir / L"ngx";
  std::error_code ec;
  fs::create_directories(log_dir, ec);
  Log(reshade::log::level::info, "Starting NGX ...");
  if (g_dlss.Init(device, g_data_dir.wstring(), log_dir.wstring())) {
    g_dlss_available = true;
    Log(reshade::log::level::info, "DLSS is available.");
  } else {
    Log(reshade::log::level::warning, "DLSS unavailable: " + g_dlss.status());
  }
  g_nvidia_ready = true;
}

void OnInitDevice(device* dev) {
  if (dev->get_api() == device_api::d3d12 && g_device == nullptr) g_device = dev;
}

void OnInitSwapchain(swapchain* sc, bool) {
  device* const dev = sc->get_device();
  if (dev == nullptr || dev->get_api() != device_api::d3d12 || g_nvidia_started.exchange(true)) return;
  if (g_device == nullptr) g_device = dev;
  g_nvidia_start_ms = GetTickCount64();
  g_nvidia_thread = std::thread(StartNvidia, reinterpret_cast<ID3D12Device*>(dev->get_native()));
}

std::string DlssStatus() {
  if (!g_nvidia_started) return "waiting for the game's swap chain";
  if (!g_nvidia_ready) return "starting NVIDIA's libraries";
  std::lock_guard lock(g_dlss_mutex);
  return g_dlss.status();
}

void OnDestroyDevice(device* dev) {
  if (dev != g_device) return;
  // NGX starts within a second or two; ReShade may unload the add-on right after this.
  if (g_nvidia_thread.joinable()) g_nvidia_thread.join();
  if (int32_t* const samples = g_taa_samples.exchange(nullptr)) samples[1] = samples[0];
  std::lock_guard lock(g_dlss_mutex);
  for (const Retired& r : g_retired) {
    if (r.view != 0) dev->destroy_resource_view(r.view);
    if (r.res != 0) dev->destroy_resource(r.res);
  }
  g_retired.clear();
  Retire(&g_motion, &g_motion_uav);
  Retire(&g_exposure, &g_exposure_uav);
  for (const Retired& r : g_retired) {
    if (r.view != 0) dev->destroy_resource_view(r.view);
    if (r.res != 0) dev->destroy_resource(r.res);
  }
  g_retired.clear();
  if (g_mv_pipeline != 0) dev->destroy_pipeline(g_mv_pipeline);
  if (g_mv_layout != 0) dev->destroy_pipeline_layout(g_mv_layout);
  g_mv_pipeline = {0};
  g_mv_layout = {0};
  {
    std::lock_guard view_lock(g_view_mutex);
    for (const auto& [res, view] : g_views) dev->destroy_resource_view(view);
    g_views.clear();
  }
  if (g_nvidia_ready) g_dlss.Shutdown();
  g_nvidia_ready = false;
  g_nvidia_started = false;
  g_dlss_available = false;
  g_device = nullptr;
}

bool KeyPressed(int vk) {
  static bool was_down[256] = {};
  if (vk <= 0 || vk >= 256) return false;
  const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
  const bool pressed = down && !was_down[vk];
  was_down[vk] = down;
  return pressed;
}

// The jitter phases DLSS wants while it runs, the game's otherwise. The game's own changes of the variable land in
// [0] first, so [0] keeps its value; the jitter reads [1].
void ApplyJitterPhases() {
  int32_t* const samples = g_taa_samples.load();
  if (samples == nullptr) return;
  const bool ours = g_config.enabled && g_config.jitter_phases > 0 && g_dlss_available;
  const int32_t want = ours ? g_config.jitter_phases : samples[0];
  if (samples[1] != want) samples[1] = want;
}

void OnPresent(command_queue* queue, swapchain*, const rect*, const rect*, uint32_t, const rect*) {
  const uint64_t frame = ++g_frame;
  static bool nvidia_late = false;
  if (!nvidia_late && !g_nvidia_ready && g_nvidia_start_ms != 0 && GetTickCount64() > g_nvidia_start_ms + 20000) {
    nvidia_late = true;
    Log(reshade::log::level::warning, "NGX has not finished starting after 20 s; the game's TAA runs until it does.");
  }
  static LARGE_INTEGER frequency = {}, last = {};
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
  if (last.QuadPart != 0)
    g_frame_time_ms = static_cast<float>(1000.0 * (now.QuadPart - last.QuadPart) / frequency.QuadPart);
  last = now;

  // Not with Alt, Ctrl or Shift held: Alt+F9 is the NVIDIA App's record shortcut.
  const bool modifier =
      ((GetAsyncKeyState(VK_MENU) | GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_SHIFT)) & 0x8000) != 0;
  if (KeyPressed(g_config.toggle_key) && !modifier) {
    SetDlssEnabled(!g_config.enabled);
    SaveConfig();
    Log(reshade::log::level::info, g_config.enabled ? "DLSS on." : "DLSS off (game TAA).");
  }
  ApplyJitterPhases();

  if (g_recreate.load() && g_nvidia_ready) {
    queue->wait_idle();
    std::lock_guard lock(g_dlss_mutex);
    g_dlss.ReleaseFeature();
    g_recreate = false;
  }
  if (g_device != nullptr) {
    std::lock_guard lock(g_dlss_mutex);
    for (auto it = g_retired.begin(); it != g_retired.end();) {
      if (frame > it->frame + 8) {
        if (it->view != 0) g_device->destroy_resource_view(it->view);
        if (it->res != 0) g_device->destroy_resource(it->res);
        it = g_retired.erase(it);
      } else {
        ++it;
      }
    }
  }
}

// --- Overlay (ReShade's Add-ons tab) ---------------------------------------------------------------------------

void OnOverlay(effect_runtime*) {
  bool enabled = g_config.enabled;
  if (ImGui::Checkbox("Use DLSS (DLAA) instead of the game's TAA", &enabled)) {
    SetDlssEnabled(enabled);
    SaveConfig();
  }
  const std::string toggle = "Toggle in game with " + KeyName(g_config.toggle_key) +
                             " (ToggleKey in Persona3ReloadDLSS.ini).";
  ImGui::SetItemTooltip("%s", toggle.c_str());

  const char* const preset_names[] = {"Default (K)", "E (older CNN model, lightest)", "J", "K", "L", "M"};
  static const int kPresetValues[] = {0, 5, 10, 11, 12, 13};
  constexpr int kPresetCount = 6;
  int index = 0;
  for (int i = 0; i < kPresetCount; ++i)
    if (kPresetValues[i] == g_config.preset) index = i;
  if (ImGui::Combo("Preset", &index, preset_names, kPresetCount)) {
    g_config.preset = kPresetValues[index];
    g_recreate = true;
    SaveConfig();
  }
  ImGui::SetItemTooltip("K: transformer model, NVIDIA's default for DLAA.\n"
                        "L and M: newer transformer models, which cost more than K (much more on RTX 20 and 30 "
                        "cards).\nE: the older CNN model, about half of K's cost.");

  if (ImGui::SliderInt("Jitter phases", &g_config.jitter_phases, 0, 32)) g_reset = true;
  if (ImGui::IsItemDeactivatedAfterEdit()) SaveConfig();
  ImGui::SetItemTooltip("While DLSS runs, the camera jitter cycles through this many sub-pixel positions "
                        "(r.TemporalAASamples). The game uses 2, which leaves DLSS too little to work with; NVIDIA "
                        "asks for at least 8. 0 keeps the game's.");
  if (ImGui::Checkbox("Game exposure", &g_config.game_exposure)) SaveConfig();
  ImGui::SetItemTooltip("Give DLSS the game's eye adaptation exposure. Off lets DLSS estimate one.");

  ImGui::Separator();
  ImGui::TextWrapped("DLSS: %s", DlssStatus().c_str());
  {
    std::lock_guard lock(g_info_mutex);
    ImGui::TextWrapped("%s", g_taa_info.c_str());
  }
  if (g_nvidia_ready) {
    if (const int32_t* const samples = g_taa_samples.load())
      ImGui::Text("Camera jitter: %d phases (the game asks for %d)", samples[1], samples[0]);
    else
      ImGui::TextWrapped("Camera jitter: the game's (r.TemporalAASamples not found: %s)", g_jitter_detail.c_str());
  }
  const uint64_t last = g_last_dlss_frame.load();
  if (g_dlss_frames.load() != 0 && last + 2 >= g_frame.load())
    ImGui::Text("Running at %ux%u, jitter %+.2f %+.2f px", g_out_width.load(), g_out_height.load(),
                g_last_jitter_x.load(), g_last_jitter_y.load());
  else if (g_config.enabled)
    ImGui::TextUnformatted("Not running this frame (the game's TAA runs).");
  if (g_failures.load() != 0) ImGui::Text("Failed evaluations: %llu", static_cast<unsigned long long>(g_failures.load()));
}

void Initialize(HMODULE module) {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(module, path, MAX_PATH);
  g_addon_dir = fs::path(path).parent_path();
  g_data_dir = g_addon_dir / L"Persona3ReloadDLSS";
  g_ini_path = g_addon_dir / L"Persona3ReloadDLSS.ini";
  LoadConfig();
  char text[160];
  std::snprintf(text, sizeof(text), "Loaded. DLSS %s; toggle key %s; %d jitter phases; preset %d.",
                g_config.enabled ? "on" : "off", KeyName(g_config.toggle_key).c_str(), g_config.jitter_phases,
                g_config.preset);
  Log(reshade::log::level::info, text);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(module)) return FALSE;
      Initialize(module);
      p3rdlss::Descriptors::RegisterEvents();
      state_tracking::register_events();
      reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
      reshade::register_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::register_event<reshade::addon_event::init_command_list>(OnInitCommandList);
      reshade::register_event<reshade::addon_event::destroy_command_list>(OnDestroyCommandList);
      reshade::register_event<reshade::addon_event::reset_command_list>(OnResetCommandList);
      reshade::register_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
      reshade::register_event<reshade::addon_event::destroy_pipeline>(OnDestroyPipeline);
      reshade::register_event<reshade::addon_event::bind_pipeline>(OnBindPipeline);
      reshade::register_event<reshade::addon_event::push_descriptors>(OnPushDescriptors);
      reshade::register_event<reshade::addon_event::dispatch>(OnDispatch);
      reshade::register_event<reshade::addon_event::destroy_resource>(OnDestroyResource);
      reshade::register_event<reshade::addon_event::present>(OnPresent);
      reshade::register_overlay(nullptr, OnOverlay);
      break;
    case DLL_PROCESS_DETACH:
      // A device the game never released leaves the finished NVIDIA thread unjoined.
      if (g_nvidia_thread.joinable()) g_nvidia_thread.detach();
      reshade::unregister_addon(module);
      break;
  }
  return TRUE;
}
