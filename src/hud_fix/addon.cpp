// Persona 3 Reload HUD fix
// ReShade add-on for HDR mods that render the game's 8-bit back buffer onto a float copy (RenoDX's resource cloning).
// Unreal's Slate HUD builds masks in the back buffer's alpha channel (subtract a shape, add one, intersect) and blends
// content through them (DstAlpha / InvDstAlpha). An 8-bit target clamps every blend result to 0..1; the float copy
// does not, and content drawn through alpha -1 comes out doubled: the bright, hard-edged shape over the minimap while
// the HUD slides in. After each draw whose alpha blend can leave 0..1, this add-on draws the same geometry again with
// an alpha-only Maximum (floor at 0) and Minimum (ceiling at 1) blend, which is what the 8-bit target did. On a real
// 8-bit target that changes nothing. docs/recon.md ("The minimap flash under RenoDX") and docs/plan.md have the
// analysis.
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "kClampAlphaCeilingPs.h"
#include "kClampAlphaFloorPs.h"

extern "C" __declspec(dllexport) const char* NAME = "Persona 3 Reload HUD fix";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Keeps the HUD's alpha masks in 0..1 when an HDR mod renders the back buffer as floats (RenoDX's bright shape "
    "over the minimap).";

using namespace reshade::api;

namespace {

namespace fs = std::filesystem;

// The clamp pipelines of one of the game's pipelines.
struct Clamps {
  pipeline floor = {0};    // alpha = max(alpha, 0)
  pipeline ceiling = {0};  // alpha = min(alpha, 1)
  bool any() const { return floor != 0 || ceiling != 0; }
};

std::shared_mutex g_mutex;
std::unordered_map<uint64_t, Clamps> g_clamps;  // the game's pipeline -> its clamp pipelines
device* g_device = nullptr;

std::atomic<bool> g_enabled{true};
std::atomic<uint64_t> g_pipelines_seen{0};    // graphics pipelines with an 8-bit color target and alpha blending
std::atomic<uint64_t> g_pipelines_clamped{0};
std::atomic<uint64_t> g_pipelines_failed{0};
std::atomic<uint64_t> g_draws_this_frame{0};  // draws given clamps since the last present
std::atomic<uint64_t> g_draws_last_frame{0};
std::atomic<uint64_t> g_draws_max_frame{0};
std::atomic<uint64_t> g_frames_with_draws{0};
std::atomic<uint64_t> g_indirect_skipped{0};  // indirect draws with clamp pipelines bound, left alone
std::atomic<bool> g_draw_events{false};
fs::path g_ini_path;

struct __declspec(uuid("2f6b8e14-5c3a-4d71-9b0e-8a4c7d2e61f3")) CmdState {
  pipeline current = {0};  // the pipeline the game bound last
  Clamps clamps;           // its clamp pipelines, if it has any
};

void Log(reshade::log::level level, const std::string& message) {
  reshade::log::message(level, ("[P3RHUD] " + message).c_str());
}

// --- Which pipelines need clamps ----------------------------------------------------------------------------------

bool IsEightBitUnorm(format f) {
  switch (f) {
    case format::b8g8r8a8_typeless:
    case format::b8g8r8a8_unorm:
    case format::b8g8r8a8_unorm_srgb:
    case format::r8g8b8a8_typeless:
    case format::r8g8b8a8_unorm:
    case format::r8g8b8a8_unorm_srgb:
      return true;
    default:
      return false;
  }
}

// A blend factor for the alpha channel, with source alpha s, destination alpha d and blend constant c (D3D: color
// factors use alpha in the alpha equation; source alpha saturate is 1 there). Negative: not covered (dual source).
float AlphaFactor(blend_factor f, float s, float d, float c) {
  switch (f) {
    case blend_factor::zero: return 0.0f;
    case blend_factor::one: return 1.0f;
    case blend_factor::source_color:
    case blend_factor::source_alpha: return s;
    case blend_factor::one_minus_source_color:
    case blend_factor::one_minus_source_alpha: return 1.0f - s;
    case blend_factor::dest_color:
    case blend_factor::dest_alpha: return d;
    case blend_factor::one_minus_dest_color:
    case blend_factor::one_minus_dest_alpha: return 1.0f - d;
    case blend_factor::constant_color:
    case blend_factor::constant_alpha: return c;
    case blend_factor::one_minus_constant_color:
    case blend_factor::one_minus_constant_alpha: return 1.0f - c;
    case blend_factor::source_alpha_saturate: return 1.0f;
    default: return -1.0f;
  }
}

// Whether render target 0's alpha blend can end below 0 or above 1 for alphas in 0..1 (an 8-bit target's inputs),
// checked on a grid of source and destination alpha and both ends of the blend constant. Unknown factors count as
// both.
void AlphaBlendRange(const blend_desc& b, bool* below, bool* above) {
  *below = *above = false;
  constexpr int kSteps = 20;
  for (int c = 0; c <= 1; ++c) {
    for (int i = 0; i <= kSteps; ++i) {
      for (int j = 0; j <= kSteps; ++j) {
        const float s = static_cast<float>(i) / kSteps, d = static_cast<float>(j) / kSteps;
        const float fs = AlphaFactor(b.source_alpha_blend_factor[0], s, d, static_cast<float>(c));
        const float fd = AlphaFactor(b.dest_alpha_blend_factor[0], s, d, static_cast<float>(c));
        float v = 0.0f;
        switch (b.alpha_blend_op[0]) {
          case blend_op::add: v = s * fs + d * fd; break;
          case blend_op::subtract: v = s * fs - d * fd; break;
          case blend_op::reverse_subtract: v = d * fd - s * fs; break;
          case blend_op::min: v = (std::min)(s, d); break;
          case blend_op::max: v = (std::max)(s, d); break;
          default: *below = *above = true; return;
        }
        if (fs < 0.0f || fd < 0.0f) {
          *below = *above = true;
          return;
        }
        if (v < -1e-4f) *below = true;
        if (v > 1.0f + 1e-4f) *above = true;
      }
    }
  }
}

// A DXIL shader (a DXBC container with a DXIL part): the clamp pipelines' pixel shader is DXIL and cannot be mixed
// with older DXBC shaders in one pipeline.
bool IsDxil(const shader_desc* s) {
  if (s == nullptr || s->code == nullptr || s->code_size < 32) return false;
  const auto* const code = static_cast<const uint8_t*>(s->code);
  if (std::memcmp(code, "DXBC", 4) != 0) return false;
  uint32_t parts = 0;
  std::memcpy(&parts, code + 28, 4);
  for (uint32_t i = 0; i < parts && 32 + 4 * (size_t{i} + 1) <= s->code_size; ++i) {
    uint32_t offset = 0;
    std::memcpy(&offset, code + 32 + 4 * i, 4);
    if (offset <= s->code_size - 4 && std::memcmp(code + offset, "DXIL", 4) == 0) return true;
  }
  return false;
}

const char* OpName(blend_op op) {
  switch (op) {
    case blend_op::add: return "add";
    case blend_op::subtract: return "subtract";
    case blend_op::reverse_subtract: return "reverse subtract";
    case blend_op::min: return "min";
    case blend_op::max: return "max";
    default: return "?";
  }
}

// A copy of the game's pipeline that draws only alpha: `value` (0 or 1) through a Maximum or Minimum blend.
pipeline CreateClamp(device* dev, pipeline_layout layout, const std::vector<pipeline_subobject>& game,
                     const blend_desc& game_blend, bool floor) {
  blend_desc blend = game_blend;
  blend.alpha_to_coverage_enable = false;
  for (int i = 0; i < 8; ++i) {
    blend.blend_enable[i] = false;
    blend.logic_op_enable[i] = false;
    blend.render_target_write_mask[i] = 0;
  }
  blend.blend_enable[0] = true;
  blend.source_color_blend_factor[0] = blend_factor::zero;
  blend.dest_color_blend_factor[0] = blend_factor::one;
  blend.color_blend_op[0] = blend_op::add;
  blend.source_alpha_blend_factor[0] = blend_factor::one;
  blend.dest_alpha_blend_factor[0] = blend_factor::one;
  blend.alpha_blend_op[0] = floor ? blend_op::max : blend_op::min;
  blend.render_target_write_mask[0] = 0x8;

  shader_desc ps = {};
  ps.code = floor ? kClampAlphaFloorPs : kClampAlphaCeilingPs;
  ps.code_size = floor ? sizeof(kClampAlphaFloorPs) : sizeof(kClampAlphaCeilingPs);

  std::vector<pipeline_subobject> objects = game;
  depth_stencil_desc depth_stencil;
  bool has_ps = false;
  for (pipeline_subobject& o : objects) {
    switch (o.type) {
      case pipeline_subobject_type::blend_state:
        o.data = &blend;
        break;
      case pipeline_subobject_type::pixel_shader:
        o.count = 1;
        o.data = &ps;
        has_ps = true;
        break;
      case pipeline_subobject_type::depth_stencil_state:
        // The same tests, so it covers the pixels the game's draw passed, but no writes.
        depth_stencil = *static_cast<const depth_stencil_desc*>(o.data);
        depth_stencil.depth_write_mask = false;
        depth_stencil.front_stencil_write_mask = 0;
        depth_stencil.back_stencil_write_mask = 0;
        o.data = &depth_stencil;
        break;
      default:
        break;
    }
  }
  if (!has_ps) objects.push_back({pipeline_subobject_type::pixel_shader, 1, &ps});
  pipeline result = {0};
  if (!dev->create_pipeline(layout, static_cast<uint32_t>(objects.size()), objects.data(), &result)) return {0};
  return result;
}

void OnInitPipeline(device* dev, pipeline_layout layout, uint32_t count, const pipeline_subobject* subobjects,
                    pipeline p) {
  const blend_desc* blend = nullptr;
  const format* targets = nullptr;
  uint32_t target_count = 0;
  const shader_desc* vs = nullptr;
  for (uint32_t i = 0; i < count; ++i) {
    switch (subobjects[i].type) {
      case pipeline_subobject_type::blend_state:
        blend = static_cast<const blend_desc*>(subobjects[i].data);
        break;
      case pipeline_subobject_type::render_target_formats:
        targets = static_cast<const format*>(subobjects[i].data);
        target_count = subobjects[i].count;
        break;
      case pipeline_subobject_type::vertex_shader:
        vs = static_cast<const shader_desc*>(subobjects[i].data);
        break;
      case pipeline_subobject_type::compute_shader:
      case pipeline_subobject_type::mesh_shader:
      case pipeline_subobject_type::amplification_shader:
      case pipeline_subobject_type::geometry_shader:
      case pipeline_subobject_type::hull_shader:
      case pipeline_subobject_type::stream_output_state:
        return;  // not a HUD draw
      default:
        break;
    }
  }
  if (blend == nullptr || targets == nullptr || target_count == 0 || vs == nullptr) return;
  if (!blend->blend_enable[0] || blend->logic_op_enable[0] || (blend->render_target_write_mask[0] & 0x8) == 0 ||
      !IsEightBitUnorm(targets[0]))
    return;
  ++g_pipelines_seen;
  bool below = false, above = false;
  AlphaBlendRange(*blend, &below, &above);
  if (!below && !above) return;
  if (!IsDxil(vs)) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) Log(reshade::log::level::warning, "A HUD pipeline is not DXIL; it keeps no clamps.");
    return;
  }

  const std::vector<pipeline_subobject> game(subobjects, subobjects + count);
  Clamps clamps;
  if (below) clamps.floor = CreateClamp(dev, layout, game, *blend, true);
  if (above) clamps.ceiling = CreateClamp(dev, layout, game, *blend, false);
  if ((below && clamps.floor == 0) || (above && clamps.ceiling == 0)) {
    if (clamps.floor != 0) dev->destroy_pipeline(clamps.floor);
    if (clamps.ceiling != 0) dev->destroy_pipeline(clamps.ceiling);
    const uint64_t failed = ++g_pipelines_failed;
    if (failed <= 4) Log(reshade::log::level::error, "Could not create the clamp pipelines for a HUD pipeline.");
    return;
  }
  {
    std::unique_lock lock(g_mutex);
    g_clamps[p.handle] = clamps;
  }
  const uint64_t clamped = ++g_pipelines_clamped;
  if (clamped <= 24) {
    char text[200];
    std::snprintf(text, sizeof(text), "Pipeline %llx: alpha blend %s (factors %d, %d) can end %s%s%s; clamps added.",
                  static_cast<unsigned long long>(p.handle), OpName(blend->alpha_blend_op[0]),
                  static_cast<int>(blend->source_alpha_blend_factor[0]),
                  static_cast<int>(blend->dest_alpha_blend_factor[0]), below ? "below 0" : "",
                  below && above ? " and " : "", above ? "above 1" : "");
    Log(reshade::log::level::info, text);
  }
}

void OnDestroyPipeline(device* dev, pipeline p) {
  Clamps clamps;
  {
    std::unique_lock lock(g_mutex);
    const auto it = g_clamps.find(p.handle);
    if (it == g_clamps.end()) return;
    clamps = it->second;
    g_clamps.erase(it);
  }
  if (clamps.floor != 0) dev->destroy_pipeline(clamps.floor);
  if (clamps.ceiling != 0) dev->destroy_pipeline(clamps.ceiling);
}

// --- Draws --------------------------------------------------------------------------------------------------------

void OnInitCommandList(command_list* cmd_list) { cmd_list->create_private_data<CmdState>(); }

void OnDestroyCommandList(command_list* cmd_list) { cmd_list->destroy_private_data<CmdState>(); }

void OnResetCommandList(command_list* cmd_list) {
  if (auto* state = cmd_list->get_private_data<CmdState>()) *state = CmdState();
}

void OnBindPipeline(command_list* cmd_list, pipeline_stage, pipeline p) {
  auto* const state = cmd_list->get_private_data<CmdState>();
  if (state == nullptr) return;
  state->current = p;
  std::shared_lock lock(g_mutex);
  const auto it = g_clamps.find(p.handle);
  state->clamps = it != g_clamps.end() ? it->second : Clamps();
}

// Records the game's draw, then the same draw with the clamp pipelines, and binds the game's pipeline again.
template <typename Draw>
bool DrawWithClamps(command_list* cmd_list, Draw&& draw) {
  auto* const state = cmd_list->get_private_data<CmdState>();
  if (state == nullptr || !state->clamps.any() || state->current == 0 || !g_enabled) return false;
  draw();
  if (state->clamps.floor != 0) {
    cmd_list->bind_pipeline(pipeline_stage::all, state->clamps.floor);
    draw();
  }
  if (state->clamps.ceiling != 0) {
    cmd_list->bind_pipeline(pipeline_stage::all, state->clamps.ceiling);
    draw();
  }
  cmd_list->bind_pipeline(pipeline_stage::all, state->current);
  ++g_draws_this_frame;
  return true;  // the game's draw is recorded already
}

bool OnDraw(command_list* cmd_list, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex,
            uint32_t first_instance) {
  return DrawWithClamps(cmd_list,
                        [&] { cmd_list->draw(vertex_count, instance_count, first_vertex, first_instance); });
}

bool OnDrawIndexed(command_list* cmd_list, uint32_t index_count, uint32_t instance_count, uint32_t first_index,
                   int32_t vertex_offset, uint32_t first_instance) {
  return DrawWithClamps(cmd_list, [&] {
    cmd_list->draw_indexed(index_count, instance_count, first_index, vertex_offset, first_instance);
  });
}

// Indirect draws are left alone (Slate draws directly); counted, in case a game does draw its HUD that way.
bool OnDrawIndirect(command_list* cmd_list, indirect_command type, resource, uint64_t, uint32_t, uint32_t) {
  if (type != indirect_command::draw && type != indirect_command::draw_indexed) return false;
  const auto* const state = cmd_list->get_private_data<CmdState>();
  if (state != nullptr && state->clamps.any()) ++g_indirect_skipped;
  return false;
}

void OnPresent(command_queue*, swapchain*, const rect*, const rect*, uint32_t, const rect*) {
  const uint64_t draws = g_draws_this_frame.exchange(0);
  g_draws_last_frame = draws;
  if (draws > g_draws_max_frame) {
    g_draws_max_frame = draws;
    Log(reshade::log::level::info, "Most clamped draws in a frame so far: " + std::to_string(draws) + ".");
  }
  if (draws != 0) ++g_frames_with_draws;
}

// The draw callbacks register once the game's swap chain exists, after every add-on has loaded, so they run after
// those of other add-ons (an HDR mod that changes state at draws sets it up before the draws are recorded here).
void OnInitSwapchain(swapchain*, bool) {
  if (g_draw_events.exchange(true)) return;
  reshade::register_event<reshade::addon_event::draw>(OnDraw);
  reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
  reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(OnDrawIndirect);
  Log(reshade::log::level::info, "Watching the HUD's draws.");
}

void OnInitDevice(device* dev) {
  if (g_device == nullptr) g_device = dev;
}

void OnDestroyDevice(device* dev) {
  if (dev != g_device) return;
  std::unique_lock lock(g_mutex);
  for (const auto& [game, clamps] : g_clamps) {
    if (clamps.floor != 0) dev->destroy_pipeline(clamps.floor);
    if (clamps.ceiling != 0) dev->destroy_pipeline(clamps.ceiling);
  }
  g_clamps.clear();
  g_device = nullptr;
}

// --- Settings and overlay -----------------------------------------------------------------------------------------

void SaveEnabled() {
  WritePrivateProfileStringW(L"Persona3ReloadHudFix", L"Enabled", g_enabled ? L"1" : L"0", g_ini_path.c_str());
}

void OnOverlay(effect_runtime*) {
  bool enabled = g_enabled;
  if (ImGui::Checkbox("Keep the HUD's alpha masks in 0..1", &enabled)) {
    g_enabled = enabled;
    SaveEnabled();
  }
  ImGui::SetItemTooltip("Off shows what the HDR mod's float copy does without it (the bright shape over the minimap "
                        "while the HUD slides in after unpausing).");
  ImGui::Text("HUD pipelines with clamps: %llu of %llu that blend alpha into an 8-bit target",
              static_cast<unsigned long long>(g_pipelines_clamped.load()),
              static_cast<unsigned long long>(g_pipelines_seen.load()));
  ImGui::Text("Clamped draws last frame: %llu (most in a frame: %llu)",
              static_cast<unsigned long long>(g_draws_last_frame.load()),
              static_cast<unsigned long long>(g_draws_max_frame.load()));
  if (g_pipelines_failed.load() != 0)
    ImGui::Text("Pipelines whose clamps could not be created: %llu",
                static_cast<unsigned long long>(g_pipelines_failed.load()));
  if (g_indirect_skipped.load() != 0)
    ImGui::Text("Indirect draws left alone: %llu", static_cast<unsigned long long>(g_indirect_skipped.load()));
}

void Initialize(HMODULE module) {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(module, path, MAX_PATH);
  g_ini_path = fs::path(path).parent_path() / L"Persona3ReloadHudFix.ini";
  g_enabled = GetPrivateProfileIntW(L"Persona3ReloadHudFix", L"Enabled", 1, g_ini_path.c_str()) != 0;
  Log(reshade::log::level::info, std::string("Loaded; clamping ") + (g_enabled ? "on." : "off."));
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(module)) return FALSE;
      Initialize(module);
      reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
      reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
      reshade::register_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::register_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
      reshade::register_event<reshade::addon_event::destroy_pipeline>(OnDestroyPipeline);
      reshade::register_event<reshade::addon_event::init_command_list>(OnInitCommandList);
      reshade::register_event<reshade::addon_event::destroy_command_list>(OnDestroyCommandList);
      reshade::register_event<reshade::addon_event::reset_command_list>(OnResetCommandList);
      reshade::register_event<reshade::addon_event::bind_pipeline>(OnBindPipeline);
      reshade::register_event<reshade::addon_event::present>(OnPresent);
      reshade::register_overlay(nullptr, OnOverlay);
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_addon(module);
      break;
  }
  return TRUE;
}
