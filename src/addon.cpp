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

#include <locale.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
#include "reflex.hpp"
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

// TAAStandalone.usf MainCS, by the digest in its DXIL container's HASH part. Two permutations with the same bindings
// and the same constants at the offsets below: the game's own, and the engine's temporal upsampling pass, which runs
// with r.TemporalAA.Upsampling 1 (here at 100%) and jitters the camera with a uniform Halton sequence.
struct TaaPermutation {
  uint8_t hash[16];
  const char* name;
};
constexpr TaaPermutation kTaaPermutations[] = {
    {{0x20, 0x31, 0x52, 0x69, 0x4a, 0xc6, 0xf5, 0xd3, 0xb9, 0x8e, 0x54, 0x64, 0x59, 0x8f, 0x56, 0x69}, "the game's TAA"},
    {{0x97, 0x9c, 0x50, 0xe3, 0x4b, 0x73, 0x23, 0xab, 0x38, 0xa4, 0x0f, 0x49, 0x66, 0x67, 0xbf, 0xfd},
     "the engine's temporal upsampling"},
    // The upsampling permutation for a render scale below 100% (logged at 67%: docs/recon.md); stock 4.27 picks the
    // permutation by the screen percentage's range.
    {{0xa8, 0x2d, 0x5a, 0xd9, 0x88, 0xf3, 0x4a, 0xe2, 0x5a, 0x19, 0x1b, 0x7f, 0x4c, 0xf3, 0x47, 0x8c},
     "the engine's temporal upsampling below 100%"},
};
constexpr int kNotTaa = -1;

// Its registers.
constexpr uint32_t kEyeAdaptationSrv = 0;  // 1x1 R32G32B32A32_FLOAT; x = the exposure the tonemapper applies
constexpr uint32_t kSceneColorSrv = 1;     // InputSceneColor, linear HDR
constexpr uint32_t kDepthSrv = 2;          // the scene depth (its depth plane), reverse Z
constexpr uint32_t kVelocitySrv = 3;       // GBufferVelocityTexture; a 1x1 dummy in the planar reflection's TAA
constexpr uint32_t kHistorySrv = 5;        // HistoryBuffer[0], at the output size
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
// View: float4 TemporalAAParams, x = the TAA sample index, y = the sample count (the engine's parameter metadata
// registers it at 0x980, as it registers TemporalAAJitter at 0x7E0 = 2016).
constexpr uint32_t kTemporalAAParams = 2432;
constexpr uint32_t kViewBegin = kClipToPrevClip;  // the part of View the add-on reads
constexpr uint32_t kViewEnd = kTemporalAAParams + 16;

// What the add-on reads from the TAA's constant buffers.
struct TaaConstants {
  bool camera_cut = false;
  float clip_to_prev_clip[16] = {};
  float jitter[2] = {};  // clip units
  float view_rect_min[2] = {};
  float view_size[4] = {};  // width, height, 1 / width, 1 / height
  float pre_exposure = 1.0f;
  float taa_params[4] = {};  // TemporalAAParams: sample index (as the passes saw it), sample count
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
  int taa = kNotTaa;       // its TAA permutation (kTaaPermutations), if it is one
  bool taa_like = false;   // development: its shader declares TAAStandalone's registers (DeclaresTaaRegisters)
  // The root constant buffers the game set (SetComputeRootConstantBufferView) for the compute root signature
  // `cbv_layout`, by root parameter. The descriptor tracking only covers descriptor tables.
  pipeline_layout cbv_layout = {0};
  std::vector<buffer_range> cbvs;
  // The same for the graphics root signature (the SSAO pass's constants; the development pass trace).
  pipeline_layout gfx_cbv_layout = {0};
  std::vector<buffer_range> gfx_cbvs;
};

// --- Settings (Persona3ReloadDLSS.ini next to the add-on) ------------------------------------------------------

struct Config {
  std::atomic<bool> enabled{true};
  int preset = 0;  // 0 = NGX's default, which is preset K for DLAA
  int toggle_key = VK_F9;
  // The engine's camera jitter phases (r.TemporalAASamples) while DLSS runs; 0 keeps the game's. The game sets 2 in
  // the places seen so far, too few for DLSS, which wants 8 or more.
  int jitter_phases = 16;
  // While DLSS runs, the engine renders through its temporal upsampling path (r.TemporalAA.Upsampling 1, at 100%),
  // which jitters the camera with a uniform Halton sequence over the whole pixel, the pattern NVIDIA trains DLSS on.
  // The game's own TAA path bends Halton into a Gaussian disc that never samples the pixel's corners.
  bool upsampling_path = true;
  // The texture mip bias on that path while DLSS runs (r.ViewTextureMipBias.Offset; the engine's default there is
  // -0.3, the game's own path uses 0). NVIDIA's DLSS guide (3.5): native bias + log2(render / display width) - 1, so
  // -1 for DLAA; less negative if fine textures shimmer, never above 0.
  float mip_bias = -1.0f;
  // NVIDIA Reflex low-latency mode (reflex.hpp), whether DLSS runs or not.
  bool reflex = true;
  // A frame rate cap in place of the game's, 0 to keep the game's. The game's frame rate option sets
  // rhi.SyncInterval (VSync on every other refresh: 120 on a 240 Hz screen) and t.MaxFPS; with a cap the add-on
  // presents on every refresh and caps through Reflex's sleep (or t.MaxFPS without Reflex).
  float frame_rate_cap = 0.0f;
  // DLSS's quality mode, the render scale while DLSS runs (r.ScreenPercentage, on the upsampling path): kModes.
  int mode = 0;
  // Development: a render scale of its own in place of the mode's (0: the mode's), and the log of TAA-like passes
  // (LogTaaPass). Both read at load.
  float render_scale = 0.0f;
  bool log_taa_passes = false;
  bool game_exposure = true;  // give DLSS the game's eye adaptation instead of letting it estimate an exposure
  int motion_mode = 0;        // development: 0 the game's velocity and camera motion, 1 camera motion only, 2 zero
  // While DLSS runs, SSAO's noise gets a random one of its 8 steps each frame instead of the jitter's (SSAO below).
  bool ssao_noise_fix = true;
  // Development: frame and shader dumps for offline analysis (DevDump below), read at load.
  bool dev_dumps = false;
};

Config g_config;

// DLSS's quality modes as render scales (NVIDIA's ratios).
struct QualityMode {
  const char* name;
  float scale;
};
constexpr QualityMode kModes[] = {
    {"DLAA (100%)", 1.0f},          {"Quality (67%)", 2.0f / 3.0f},          {"Balanced (58%)", 0.58f},
    {"Performance (50%)", 0.5f}, {"Ultra Performance (33%)", 1.0f / 3.0f},
};
constexpr int kModeCount = static_cast<int>(std::size(kModes));

// The render scale while DLSS runs: the development override, else the mode's.
float RenderScale() {
  return g_config.render_scale > 0.0f ? g_config.render_scale : kModes[g_config.mode].scale;
}
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
// r.TemporalAA.Upsampling's values in the same layout, and what they were when found (put back whenever DLSS does not
// run, so F9 compares with the game's own TAA path); g_upsampling_detail says how, readable once g_nvidia_ready.
std::atomic<int32_t*> g_taa_upsampling{nullptr};
int32_t g_upsampling_game[2] = {0, 0};
std::string g_upsampling_detail;
// r.ViewTextureMipBias.Offset (a float variable) in the same layout and the same way.
std::atomic<float*> g_mip_bias_offset{nullptr};
float g_mip_bias_game[2] = {0.0f, 0.0f};
std::string g_mip_bias_detail;

// An engine variable the add-on overrides at times while the game may change it too (from its options): the game's
// own values are followed (a value the add-on did not write is the game's) and put back when the override ends.
template <typename T>
struct EngineVariable {
  std::atomic<T*> values{nullptr};  // [0] the game thread's copy, [1] the render thread's
  T game[2] = {};                   // the game's values
  T written[2] = {};                // what the values were after the last Apply
  bool overriding = false;
  uint64_t game_writes = 0;         // how often the game wrote it (while overridden: how often it fought the override)
  std::string detail;               // how it was found, readable once g_nvidia_ready

  void Found(T* v) {
    game[0] = written[0] = v[0];
    game[1] = written[1] = v[1];
    values = v;
  }
  // `want` while `ours`, the game's values otherwise. True when the game changed its values since the last call.
  bool Apply(bool ours, T want) {
    T* const v = values.load();
    if (v == nullptr) return false;
    bool changed = false;
    for (int i = 0; i < 2; ++i) {
      if (v[i] != written[i]) {  // the game wrote it
        changed = changed || v[i] != game[i];
        game[i] = v[i];
        ++game_writes;
      }
      const T target = ours ? want : game[i];
      if (v[i] != target) v[i] = target;
      written[i] = target;
    }
    overriding = ours;
    return changed;
  }
  void Restore() {
    if (T* const v = values.exchange(nullptr)) {
      v[0] = game[0];
      v[1] = game[1];
    }
  }
};
EngineVariable<float> g_max_fps;       // t.MaxFPS: the engine's frame rate limit, 0 for none
EngineVariable<int32_t> g_sync_interval;  // rhi.SyncInterval: present on every n-th refresh while VSync is on
EngineVariable<float> g_screen_percentage;  // r.ScreenPercentage: the render scale in percent (development, stage 3)
// r.AmbientOcclusionLevels: SSAO's levels (-1 automatic, 0 off). Development: off from the overlay, to test whether
// the 4x4 grid DLSS shows on skin and in corners comes from SSAO's noise (DevDumps=1).
EngineVariable<int32_t> g_ao_levels;
std::atomic<bool> g_dev_no_ssao{false};

p3rdlss::Reflex g_reflex;  // started by the NVIDIA thread; g_reflex_ready once it has run
std::atomic<bool> g_reflex_ready{false};
std::atomic<int> g_last_permutation{kNotTaa};  // the TAA permutation DLSS replaced last

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
std::atomic<uint32_t> g_render_width{0}, g_render_height{0};  // DLSS's input size last time it ran
std::atomic<float> g_last_jitter_x{0.0f}, g_last_jitter_y{0.0f};
// DLSS frames whose jitter sat at the pixel's corner (-0.5, -0.5), which no Halton point or the game's disc reaches:
// the engine's signed-byte sample index wrapped past 127 (more than 128 phases, docs/recon.md).
std::atomic<uint64_t> g_corner_jitter_frames{0};

std::shared_mutex g_taa_mutex;
std::unordered_map<uint64_t, int> g_taa_pipelines;  // pipeline handle -> TAA permutation
// Development (RenderScale > 0 at load): the digests of the compute pipelines whose shaders declare TAAStandalone's
// registers (DeclaresTaaRegisters), the only ones whose dispatches LogTaaPass looks at.
using Digest = std::array<uint8_t, 16>;
bool g_diagnostics = false;
std::unordered_map<uint64_t, Digest> g_compute_digests;
std::mutex g_logged_passes_mutex;
std::unordered_set<std::string> g_logged_passes;  // the TAA-like passes logged already (digest and sizes)

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

// Decimal numbers in the ini always use a point, whatever the locale.
_locale_t CLocale() {
  static const _locale_t locale = _create_locale(LC_NUMERIC, "C");
  return locale;
}

float ReadFloat(const wchar_t* key, float fallback) {
  wchar_t text[32] = {};
  GetPrivateProfileStringW(L"Persona3ReloadDLSS", key, L"", text, 32, g_ini_path.c_str());
  wchar_t* end = nullptr;
  const float value = _wcstof_l(text, &end, CLocale());
  return end != text && std::isfinite(value) ? value : fallback;
}

void WriteFloat(const wchar_t* key, float value) {
  wchar_t text[32];
  _swprintf_s_l(text, 32, L"%.2f", CLocale(), value);
  WritePrivateProfileStringW(L"Persona3ReloadDLSS", key, text, g_ini_path.c_str());
}

void LoadConfig() {
  g_config.enabled = ReadInt(L"Enabled", 1) != 0;
  g_config.preset = ReadInt(L"Preset", 0);
  g_config.toggle_key = ReadInt(L"ToggleKey", VK_F9);
  g_config.jitter_phases = std::clamp(ReadInt(L"JitterPhases", 16), 0, 64);
  g_config.upsampling_path = ReadInt(L"UpsamplingPath", 1) != 0;
  g_config.mip_bias = std::clamp(ReadFloat(L"MipBias", -1.0f), -2.0f, 0.0f);
  g_config.reflex = ReadInt(L"Reflex", 1) != 0;
  g_config.frame_rate_cap = ReadFloat(L"FrameRateCap", 0.0f);
  if (g_config.frame_rate_cap != 0.0f) g_config.frame_rate_cap = std::clamp(g_config.frame_rate_cap, 30.0f, 1000.0f);
  g_config.mode = std::clamp(ReadInt(L"Mode", 0), 0, kModeCount - 1);
  g_config.render_scale = ReadFloat(L"RenderScale", 0.0f);
  if (g_config.render_scale != 0.0f) g_config.render_scale = std::clamp(g_config.render_scale, 0.33f, 1.0f);
  g_config.log_taa_passes = ReadInt(L"LogTaaPasses", 0) != 0;
  g_config.dev_dumps = ReadInt(L"DevDumps", 0) != 0;
  g_config.ssao_noise_fix = ReadInt(L"SsaoNoiseFix", 1) != 0;
  g_config.game_exposure = ReadInt(L"GameExposure", 1) != 0;
  g_config.motion_mode = std::clamp(ReadInt(L"MotionVectors", 0), 0, 2);
}

void SaveConfig() {
  WriteInt(L"Enabled", g_config.enabled);
  WriteInt(L"Preset", g_config.preset);
  WriteInt(L"ToggleKey", g_config.toggle_key);
  WriteInt(L"JitterPhases", g_config.jitter_phases);
  WriteInt(L"UpsamplingPath", g_config.upsampling_path);
  WriteFloat(L"MipBias", g_config.mip_bias);
  WriteInt(L"Reflex", g_config.reflex);
  WriteFloat(L"FrameRateCap", g_config.frame_rate_cap);
  WriteInt(L"Mode", g_config.mode);
  WriteInt(L"GameExposure", g_config.game_exposure);
  WriteInt(L"MotionVectors", g_config.motion_mode);
  WriteInt(L"SsaoNoiseFix", g_config.ssao_noise_fix);
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
// four-character code and size. The HASH part holds flags and the shader's digest, which goes to `out`.
bool DxilDigest(const shader_desc* cs, Digest* out) {
  if (cs == nullptr || cs->code == nullptr || cs->code_size < 32) return false;
  const auto* const code = static_cast<const uint8_t*>(cs->code);
  if (std::memcmp(code, "DXBC", 4) != 0) return false;
  uint32_t parts = 0;
  std::memcpy(&parts, code + 28, 4);
  for (uint32_t i = 0; i < parts && 32 + 4 * (size_t{i} + 1) <= cs->code_size; ++i) {
    uint32_t offset = 0;
    std::memcpy(&offset, code + 32 + 4 * i, 4);
    if (offset > cs->code_size || cs->code_size - offset < 28) continue;
    if (std::memcmp(code + offset, "HASH", 4) != 0) continue;
    std::memcpy(out->data(), code + offset + 12, 16);
    return true;
  }
  return false;
}

// Whether a DXIL compute shader declares TAAStandalone's registers: typed SRVs t0 to t5 (eye adaptation, color, depth,
// velocity, stencil, history), a typed UAV u0 and constant buffers b0 and b1, all in space 0 (LogTaaPass reads all of
// them). From the container's PSV0 part (pipeline state
// validation): its runtime info size and info, the resource count, the size of one binding and the bindings (type,
// space, lower and upper register). Only such shaders' bindings are looked at in the log: the registers a shader
// does not declare can hold stale descriptors of textures that no longer exist.
bool DeclaresTaaRegisters(const shader_desc* cs) {
  if (cs == nullptr || cs->code == nullptr || cs->code_size < 32) return false;
  const auto* const code = static_cast<const uint8_t*>(cs->code);
  if (std::memcmp(code, "DXBC", 4) != 0) return false;
  uint32_t parts = 0;
  std::memcpy(&parts, code + 28, 4);
  for (uint32_t i = 0; i < parts && 32 + 4 * (size_t{i} + 1) <= cs->code_size; ++i) {
    uint32_t offset = 0, size = 0;
    std::memcpy(&offset, code + 32 + 4 * i, 4);
    if (offset > cs->code_size || cs->code_size - offset < 8 || std::memcmp(code + offset, "PSV0", 4) != 0) continue;
    std::memcpy(&size, code + offset + 4, 4);
    const uint8_t* p = code + offset + 8;
    const uint8_t* const end = p + (std::min)(static_cast<size_t>(size), cs->code_size - offset - 8);
    const auto read = [&](uint32_t* v) {
      if (end - p < 4) return false;
      std::memcpy(v, p, 4);
      p += 4;
      return true;
    };
    uint32_t info_size = 0, count = 0, bind_size = 0;
    if (!read(&info_size) || static_cast<size_t>(end - p) < info_size) return false;
    p += info_size;
    if (!read(&count) || count == 0 || !read(&bind_size) || bind_size < 16 ||
        static_cast<size_t>(end - p) < static_cast<size_t>(count) * bind_size)
      return false;
    constexpr uint32_t kCbv = 2, kSrvTyped = 3, kUavTyped = 6;
    bool srv[6] = {}, uav0 = false, cbv[2] = {};
    for (uint32_t r = 0; r < count; ++r, p += bind_size) {
      uint32_t type, space, lower, upper;
      std::memcpy(&type, p, 4);
      std::memcpy(&space, p + 4, 4);
      std::memcpy(&lower, p + 8, 4);
      std::memcpy(&upper, p + 12, 4);
      if (space != 0) continue;
      const auto covers = [&](uint32_t reg) { return lower <= reg && reg <= upper; };
      if (type == kSrvTyped)
        for (uint32_t t = 0; t < 6; ++t) srv[t] = srv[t] || covers(t);
      if (type == kUavTyped) uav0 = uav0 || covers(0);
      if (type == kCbv) {
        cbv[0] = cbv[0] || covers(0);
        cbv[1] = cbv[1] || covers(1);
      }
    }
    return srv[0] && srv[1] && srv[2] && srv[3] && srv[4] && srv[5] && uav0 && cbv[0] && cbv[1];
  }
  return false;
}

// The TAA permutation of a compute shader, by its digest.
int FindTaaPermutation(const Digest& digest) {
  for (int p = 0; p < static_cast<int>(std::size(kTaaPermutations)); ++p)
    if (std::memcmp(digest.data(), kTaaPermutations[p].hash, 16) == 0) return p;
  return kNotTaa;
}

std::string Hex(const Digest& digest) {
  char text[33];
  for (int i = 0; i < 16; ++i) std::snprintf(text + 2 * i, 3, "%02x", digest[i]);
  return text;
}

// --- Development: frame and shader dumps (DevDumps=1) ----------------------------------------------------------

// For offline analysis without a graphics debugger (RenderDoc runs out of memory in this game): every pixel and
// compute shader the game creates goes once to Persona3ReloadDLSS\dev\shaders\<ps|cs>_<digest>.dxil, and when a file
// Persona3ReloadDLSS\dev\dump.txt appears (holding a frame count, default 4; looked for every 30 frames), the next
// DLSS frames' TAA input color (render size) and DLSS output are copied to readback buffers and written to dev\frames
// as raw RGBA16F rows (<frame>_color_<w>x<h>.bin, <frame>_output_<w>x<h>.bin), with a line of frames.jsonl each
// (sizes, jitter, pre-exposure, jitter phases, reset). The copies use the states DLSS needs those resources in: the
// color as a non-pixel shader resource, the output as an unordered access view.
fs::path g_dev_dir;
std::mutex g_dev_mutex;
std::unordered_set<std::string> g_dev_shaders;  // shaders written so far
std::atomic<int> g_dev_frames_left{0};          // DLSS frames still to dump

struct DevCopy {
  resource buffer = {0};
  uint32_t width = 0, height = 0, row_pixels = 0;
  uint64_t size = 0;
  std::string name;
};
struct DevFrame {
  uint64_t frame = 0;
  std::vector<DevCopy> copies;
  std::string json;
};
std::vector<DevFrame> g_dev_pending;  // copies recorded, not yet read back

FILE* DevOpen(const fs::path& path, const wchar_t* mode) {
  FILE* file = nullptr;
  return _wfopen_s(&file, path.c_str(), mode) == 0 ? file : nullptr;
}

std::unordered_map<uint64_t, std::string> g_dev_pipeline_shaders;  // pipeline -> "ps_<digest>" / "cs_<digest>"

void DevDumpShaders(uint32_t count, const pipeline_subobject* subobjects, pipeline p) {
  for (uint32_t i = 0; i < count; ++i) {
    const char* const stage = subobjects[i].type == pipeline_subobject_type::pixel_shader     ? "ps"
                              : subobjects[i].type == pipeline_subobject_type::compute_shader ? "cs"
                                                                                              : nullptr;
    if (stage == nullptr || subobjects[i].data == nullptr) continue;
    const auto* const desc = static_cast<const shader_desc*>(subobjects[i].data);
    Digest digest = {};
    if (!DxilDigest(desc, &digest)) continue;
    const std::string name = std::string(stage) + "_" + Hex(digest) + ".dxil";
    {
      std::lock_guard lock(g_dev_mutex);
      std::string& shaders = g_dev_pipeline_shaders[p.handle];
      shaders += (shaders.empty() ? "" : ",") + name.substr(0, name.size() - 5);
      if (!g_dev_shaders.insert(name).second) continue;
    }
    if (FILE* const f = DevOpen(g_dev_dir / L"shaders" / name, L"wb")) {
      std::fwrite(desc->code, 1, desc->code_size, f);
      std::fclose(f);
    }
  }
}

// Records copies of this DLSS frame's input color and output while a dump is under way.
void DevDumpFrame(command_list* cmd, resource color, uint32_t render_width, uint32_t render_height, resource output,
                  uint32_t width, uint32_t height, const std::string& json) {
  if (g_dev_frames_left.fetch_sub(1) <= 0) {
    g_dev_frames_left = 0;
    return;
  }
  device* const dev = cmd->get_device();
  DevFrame frame;
  frame.frame = g_frame.load();
  frame.json = json;
  const auto copy = [&](resource src, resource_usage state, uint32_t w, uint32_t h, const char* name) {
    const uint32_t row = (w + 31) / 32 * 32;  // 8 bytes a pixel: rows of a multiple of 256 bytes, as D3D12 wants
    const uint64_t size = uint64_t{row} * h * 8;
    resource buffer = {0};
    if (!dev->create_resource(resource_desc(size, memory_heap::readback, resource_usage::copy_dest), nullptr,
                              resource_usage::copy_dest, &buffer))
      return;
    cmd->barrier(src, state, resource_usage::copy_source);
    const subresource_box box = {0, 0, 0, w, h, 1};
    cmd->copy_texture_to_buffer(src, 0, &box, buffer, 0, row, h);
    cmd->barrier(src, resource_usage::copy_source, state);
    frame.copies.push_back({buffer, w, h, row, size, name});
  };
  copy(color, resource_usage::shader_resource_non_pixel, render_width, render_height, "color");
  copy(output, resource_usage::unordered_access, width, height, "output");
  std::lock_guard lock(g_dev_mutex);
  g_dev_pending.push_back(std::move(frame));
}

bool ReadBuffer(device* dev, const buffer_range& range, uint32_t offset, uint32_t size, void* out);  // below

// --- SSAO's noise, unlocked from the camera jitter -------------------------------------------------------------

// Unreal's SSAO rotates its samples with a 4x4 noise texture that it shifts every frame by TemporalOffset =
// (TAA sample index % 8) * (2.48, 7.52) / 64 (ScreenSpaceAOParams[3].xy: this game's SSAO pixel shader reads it from
// row 30 of its $Globals, next to the inverse tangent of half the field of view in .w; the vertex shader's copy is at
// row 14). The sample index also picks the camera jitter, so every jitter position always meets the same noise step:
// the game's TAA blurs the steps together, DLSS puts each frame's samples at their jitter positions and keeps the
// noise as a fine 4x4 grid on skin and in corners (Junpei's cheek; docs/recon.md). While DLSS runs, the SSAO pass (the
// frame's full-screen draw into an R8 target at the render size) gets a random one of the same 8 steps each frame, in
// every constant buffer bound for it that holds the offset; the step's rows are remembered per pipeline for the frames
// whose step is 0 (an offset of 0 cannot be told from other zeros).
constexpr float kAoStepX = 2.48f / 64.0f, kAoStepY = 7.52f / 64.0f;
constexpr uint32_t kAoRows = 48;  // rows of each constant buffer looked at
std::atomic<uint64_t> g_ao_frames{0};      // frames whose SSAO step the add-on chose
std::atomic<uint64_t> g_ao_last_frame{0};
std::atomic<int> g_ao_from{-1}, g_ao_to{-1};
std::atomic<uint32_t> g_ao_rows{0};        // copies of the offset changed in the last such frame
std::mutex g_ao_mutex;
std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>> g_ao_locations;  // pipeline -> (root param, row)

uint64_t Mix64(uint64_t x) {  // splitmix64's finalizer
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ull;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

// A row (x, y, z, w) holding the SSAO offset at step 1 to 7: (k * 2.48 / 64, k * 7.52 / 64, ..., 1 / tan(fov / 2)).
bool AoOffsetRow(const float* row, int* step) {
  if (!(row[3] > 0.2f && row[3] < 10.0f)) return false;
  const float kx = row[0] / kAoStepX, ky = row[1] / kAoStepY;
  const long k = std::lround(kx);
  if (k < 1 || k > 7 || std::fabs(kx - static_cast<float>(k)) > 1e-3f || std::fabs(ky - static_cast<float>(k)) > 1e-3f)
    return false;
  *step = static_cast<int>(k);
  return true;
}

void RandomizeSsaoNoise(command_list* cmd, const CmdData& data) {
  device* const dev = cmd->get_device();
  const uint64_t frame = g_frame.load();
  std::vector<std::pair<uint32_t, uint32_t>> found;
  int from = 0;
  for (uint32_t param = 0; param < data.gfx_cbvs.size(); ++param) {
    if (data.gfx_cbvs[param].buffer == 0) continue;
    float rows[kAoRows * 4];
    uint32_t count = kAoRows;
    while (count >= 16 && !ReadBuffer(dev, data.gfx_cbvs[param], 0, count * 16, rows)) count /= 2;
    if (count < 16) continue;
    for (uint32_t r = 0; r < count; ++r) {
      int step = 0;
      if (AoOffsetRow(rows + 4 * r, &step)) {
        found.emplace_back(param, r);
        from = step;
      }
    }
  }
  {
    std::lock_guard lock(g_ao_mutex);
    if (!found.empty())
      g_ao_locations[data.current.handle] = found;
    else if (const auto it = g_ao_locations.find(data.current.handle); it != g_ao_locations.end())
      found = it->second;  // step 0 this frame
  }
  if (found.empty()) return;
  const int to = static_cast<int>(Mix64(frame * 0x9E3779B97F4A7C15ull) % 8);
  const float offset[2] = {to * kAoStepX, to * kAoStepY};
  uint32_t written = 0;
  for (const auto& [param, r] : found) {
    if (param >= data.gfx_cbvs.size() || data.gfx_cbvs[param].buffer == 0) continue;
    const buffer_range& range = data.gfx_cbvs[param];
    const resource_desc desc = dev->get_resource_desc(range.buffer);
    if (desc.type != resource_type::buffer || desc.heap != memory_heap::upload ||
        range.offset + r * 16 + sizeof(offset) > desc.buffer.size)
      continue;
    void* mapped = nullptr;
    if (dev->map_buffer_region(range.buffer, range.offset + r * 16, sizeof(offset), map_access::write_only, &mapped) &&
        mapped != nullptr) {
      std::memcpy(mapped, offset, sizeof(offset));
      dev->unmap_buffer_region(range.buffer);
      ++written;
    }
  }
  g_ao_from = from;
  g_ao_to = to;
  g_ao_rows = written;
  g_ao_last_frame = frame;
  if (written != 0 && ++g_ao_frames == 1) {
    char text[200];
    std::snprintf(text, sizeof(text),
                  "SSAO: its noise gets a random step from here on (frame %llu: step %d became %d in %u constant "
                  "buffer row(s)).",
                  static_cast<unsigned long long>(frame), from, to, written);
    Log(reshade::log::level::info, text);
  }
}

// The SSAO pass: a full-screen triangle into one R8 target at DLSS's render size, while DLSS runs.
void SsaoDraw(command_list* cmd, uint32_t vertices, uint32_t instances) {
  if (!g_config.ssao_noise_fix || !g_config.enabled || vertices != 3 || instances != 1 ||
      g_last_dlss_frame.load() + 2 < g_frame.load())
    return;
  auto* const state = cmd->get_private_data<state_tracking>();
  auto* const data = cmd->get_private_data<CmdData>();
  if (state == nullptr || data == nullptr || state->render_targets.size() != 1 || state->render_targets[0] == 0) return;
  device* const dev = cmd->get_device();
  const format f = dev->get_resource_view_desc(state->render_targets[0]).format;
  if (f != format::r8_unorm && f != format::r8_typeless) return;
  const resource_desc desc = dev->get_resource_desc(dev->get_resource_from_view(state->render_targets[0]));
  if (desc.texture.width != g_render_width.load() || desc.texture.height != g_render_height.load()) return;
  RandomizeSsaoNoise(cmd, *data);
}

// A trace of one frame's passes (dev\trace.txt arms it for the next frame; dev\trace_<frame>.txt): every draw and
// dispatch with its pipeline's shaders, render targets (format, size) and root constant buffers; for dispatches and
// full-screen draws (at most 4 vertices, one instance) also the first 16 rows of each constant buffer, as floats.
std::atomic<uint64_t> g_trace_frame{0};
std::atomic<uint64_t> g_trace_seq{0};
std::mutex g_trace_mutex;
std::vector<std::pair<uint64_t, std::string>> g_trace_lines;

void DevTrace(command_list* cmd, bool dispatch, uint32_t a, uint32_t b, uint32_t c) {
  if (g_trace_frame.load(std::memory_order_relaxed) != g_frame.load(std::memory_order_relaxed)) return;
  device* const dev = cmd->get_device();
  auto* const data = cmd->get_private_data<CmdData>();
  auto* const state = cmd->get_private_data<state_tracking>();
  if (data == nullptr) return;
  const uint64_t seq = g_trace_seq++;
  char head[96];
  std::snprintf(head, sizeof(head), "%llu t%lu %s %u %u %u", static_cast<unsigned long long>(seq), GetCurrentThreadId(),
                dispatch ? "dispatch" : "draw", a, b, c);
  std::string line = head;
  {
    std::lock_guard lock(g_dev_mutex);
    const auto it = g_dev_pipeline_shaders.find(data->current.handle);
    line += " " + (it != g_dev_pipeline_shaders.end() ? it->second : std::string("?"));
  }
  if (!dispatch && state != nullptr) {
    for (const resource_view rtv : state->render_targets) {
      if (rtv == 0) continue;
      const resource res = dev->get_resource_from_view(rtv);
      const resource_desc desc = dev->get_resource_desc(res);
      char rt[64];
      std::snprintf(rt, sizeof(rt), " rt=%d:%ux%u", static_cast<int>(dev->get_resource_view_desc(rtv).format),
                    desc.texture.width, desc.texture.height);
      line += rt;
    }
  }
  const bool contents = dispatch || (a <= 4 && b == 1);
  const std::vector<buffer_range>& cbvs = dispatch ? data->cbvs : data->gfx_cbvs;
  for (size_t i = 0; i < cbvs.size(); ++i) {
    if (cbvs[i].buffer == 0) continue;
    char cb[64];
    std::snprintf(cb, sizeof(cb), " cb%zu=%llx+%llu", i, static_cast<unsigned long long>(cbvs[i].buffer.handle),
                  static_cast<unsigned long long>(cbvs[i].offset));
    line += cb;
    float rows[64];
    if (contents && ReadBuffer(dev, cbvs[i], 0, sizeof(rows), rows)) {
      line += "[";
      for (int k = 0; k < 64; ++k) {
        char v[24];
        std::snprintf(v, sizeof(v), k == 0 ? "%.6g" : " %.6g", rows[k]);
        line += v;
      }
      line += "]";
    }
  }
  std::lock_guard lock(g_trace_mutex);
  g_trace_lines.emplace_back(seq, std::move(line));
}

bool OnDevDraw(command_list* cmd, uint32_t vertices, uint32_t instances, uint32_t, uint32_t) {
  SsaoDraw(cmd, vertices, instances);
  if (g_config.dev_dumps) DevTrace(cmd, false, vertices, instances, 0);
  return false;
}
bool OnDevDrawIndexed(command_list* cmd, uint32_t indices, uint32_t instances, uint32_t, int32_t, uint32_t) {
  if (g_config.dev_dumps) DevTrace(cmd, false, indices, instances, 1);
  return false;
}

// At the present: arms a dump when the trigger file appears, and writes the dumps whose frames the GPU has finished.
void DevDumpPresent(command_queue* queue, uint64_t frame) {
  if (g_trace_frame.load() != 0 && g_trace_frame.load() < frame) {  // the traced frame has ended
    std::vector<std::pair<uint64_t, std::string>> lines;
    {
      std::lock_guard lock(g_trace_mutex);
      lines.swap(g_trace_lines);
    }
    std::sort(lines.begin(), lines.end());
    char name[48];
    std::snprintf(name, sizeof(name), "trace_%06llu.txt", static_cast<unsigned long long>(g_trace_frame.load()));
    if (FILE* const out = DevOpen(g_dev_dir / name, L"wb")) {
      for (const auto& [seq, line] : lines) {
        std::fputs(line.c_str(), out);
        std::fputc('\n', out);
      }
      std::fclose(out);
    }
    Log(reshade::log::level::info, "Development: traced frame " + std::to_string(g_trace_frame.load()) + ": " +
                                       std::to_string(lines.size()) + " draws and dispatches.");
    g_trace_frame = 0;
  }
  if (frame % 30 == 15) {
    std::error_code error;
    if (fs::exists(g_dev_dir / L"trace.txt", error)) {
      fs::remove(g_dev_dir / L"trace.txt", error);
      g_trace_seq = 0;
      g_trace_frame = frame;  // the frame that starts now
    }
  }
  if (frame % 30 == 0) {
    const fs::path trigger = g_dev_dir / L"dump.txt";
    std::error_code error;
    if (fs::exists(trigger, error)) {
      int count = 4;
      if (FILE* const f = DevOpen(trigger, L"rb")) {
        char text[16] = {};
        const int n = std::fgets(text, sizeof(text), f) != nullptr ? std::atoi(text) : 0;
        if (n > 0 && n <= 32) count = n;
        std::fclose(f);
      }
      fs::remove(trigger, error);
      g_dev_frames_left = count;
      Log(reshade::log::level::info, "Development: dumping the next " + std::to_string(count) + " DLSS frames.");
    }
  }
  std::vector<DevFrame> ready;
  {
    std::lock_guard lock(g_dev_mutex);
    for (auto it = g_dev_pending.begin(); it != g_dev_pending.end();) {
      if (frame > it->frame + 2) {
        ready.push_back(std::move(*it));
        it = g_dev_pending.erase(it);
      } else {
        ++it;
      }
    }
  }
  if (ready.empty()) return;
  queue->wait_idle();
  device* const dev = queue->get_device();
  for (DevFrame& f : ready) {
    for (DevCopy& c : f.copies) {
      void* data = nullptr;
      if (dev->map_buffer_region(c.buffer, 0, c.size, map_access::read_only, &data) && data != nullptr) {
        char name[96];
        std::snprintf(name, sizeof(name), "%06llu_%s_%ux%u.bin", static_cast<unsigned long long>(f.frame),
                      c.name.c_str(), c.width, c.height);
        if (FILE* const out = DevOpen(g_dev_dir / L"frames" / name, L"wb")) {
          for (uint32_t y = 0; y < c.height; ++y)
            std::fwrite(static_cast<const uint8_t*>(data) + size_t{y} * c.row_pixels * 8, 8, c.width, out);
          std::fclose(out);
        }
        dev->unmap_buffer_region(c.buffer);
      }
      dev->destroy_resource(c.buffer);
    }
    if (FILE* const out = DevOpen(g_dev_dir / L"frames" / L"frames.jsonl", L"ab")) {
      std::fputs((f.json + "\n").c_str(), out);
      std::fclose(out);
    }
    Log(reshade::log::level::info, "Development: dumped DLSS frame " + std::to_string(f.frame) + ".");
  }
}

void OnInitPipeline(device*, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects, pipeline p) {
  if (g_config.dev_dumps) DevDumpShaders(count, subobjects, p);
  for (uint32_t i = 0; i < count; ++i) {
    if (subobjects[i].type != pipeline_subobject_type::compute_shader) continue;
    const auto* const cs = static_cast<const shader_desc*>(subobjects[i].data);
    Digest digest = {};
    if (!DxilDigest(cs, &digest)) return;
    if (g_diagnostics && DeclaresTaaRegisters(cs)) {
      std::unique_lock lock(g_taa_mutex);
      g_compute_digests[p.handle] = digest;
    }
    const int permutation = FindTaaPermutation(digest);
    if (permutation == kNotTaa) return;
    size_t total = 0;
    {
      std::unique_lock lock(g_taa_mutex);
      g_taa_pipelines[p.handle] = permutation;
      total = g_taa_pipelines.size();
    }
    char text[160];
    std::snprintf(text, sizeof(text), "Found the TAA shader of %s (pipeline %llx, %zu so far).",
                  kTaaPermutations[permutation].name, static_cast<unsigned long long>(p.handle), total);
    Log(reshade::log::level::info, text);
    return;
  }
}

void OnDestroyPipeline(device*, pipeline p) {
  std::unique_lock lock(g_taa_mutex);
  g_taa_pipelines.erase(p.handle);
  g_compute_digests.erase(p.handle);
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
  const auto it = g_taa_pipelines.find(p.handle);
  data->taa = it != g_taa_pipelines.end() ? it->second : kNotTaa;
  data->taa_like = g_diagnostics && g_compute_digests.count(p.handle) != 0;
}

void OnPushDescriptors(command_list* cmd_list, shader_stage stages, pipeline_layout layout, uint32_t param,
                       const descriptor_table_update& update) {
  if ((stages & shader_stage::all_graphics) != 0 &&
      update.type == descriptor_type::constant_buffer && update.count == 1 && update.descriptors != nullptr) {
    if (auto* const data = cmd_list->get_private_data<CmdData>()) {
      if (data->gfx_cbv_layout != layout) {
        data->gfx_cbv_layout = layout;
        data->gfx_cbvs.clear();
      }
      if (param >= data->gfx_cbvs.size()) data->gfx_cbvs.resize(param + 1);
      data->gfx_cbvs[param] = static_cast<const buffer_range*>(update.descriptors)[0];
    }
  }
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
  std::memcpy(out->taa_params, at(kTemporalAAParams), sizeof(out->taa_params));
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
  const resource history = bound(RegisterKind::kShaderResource, kHistorySrv);
  const resource output = bound(RegisterKind::kUnorderedAccess, kOutputUav);
  const resource_desc od = output != 0 ? dev->get_resource_desc(output) : resource_desc();
  const uint32_t width = od.texture.width, height = od.texture.height;  // the output size
  // The inputs' size: the render view sits in their top-left corner (the scene textures can be a little larger).
  const resource_desc cd = color != 0 ? dev->get_resource_desc(color) : resource_desc();
  const uint32_t in_width = cd.texture.width, in_height = cd.texture.height;
  const format depth_format = depth != 0 ? DepthViewFormat(dev->get_resource_desc(depth).texture.format) : format::unknown;
  g_out_width = width;
  g_out_height = height;

  TaaConstants c;
  std::string why;
  bool valid = ReadTaaConstants(dev, *descriptors, data, tables->first, &c, &why);
  // The render size, and the jitter in render pixels, x right and y down (TemporalAAJitter is in clip units of the
  // render view).
  const auto render_width = static_cast<uint32_t>(c.view_size[0]), render_height = static_cast<uint32_t>(c.view_size[1]);
  const float jitter_x = c.jitter[0] * 0.5f * c.view_size[0];
  const float jitter_y = -c.jitter[1] * 0.5f * c.view_size[1];
  if (valid && !(IsTexture(dev, output, width, height, format::r16g16b16a16_typeless) &&
                 IsTexture(dev, history, width, height, format::r16g16b16a16_typeless) &&
                 IsTexture(dev, color, in_width, in_height, format::r16g16b16a16_typeless) &&
                 IsTexture(dev, velocity, in_width, in_height, format::r16g16b16a16_typeless) &&
                 IsTexture(dev, depth, in_width, in_height, format::unknown) && depth_format != format::unknown &&
                 IsTexture(dev, eye, 1, 1, format::r32g32b32a32_typeless))) {
    valid = false;
    why = "unexpected textures";
  }
  if (valid && !(c.view_rect_min[0] == 0.0f && c.view_rect_min[1] == 0.0f && render_width >= 64 &&
                 render_height >= 64 && render_width <= in_width && render_height <= in_height &&
                 render_width <= width && render_height <= height &&
                 static_cast<float>(render_width) == c.view_size[0] &&
                 static_cast<float>(render_height) == c.view_size[1])) {
    valid = false;
    why = "the render view is not in the inputs' top-left corner or larger than the output";
  }
  const float scale = valid ? static_cast<float>(render_width) / static_cast<float>(width) : 1.0f;
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
  if (!g_dlss.available() || g_recreate.load() || !EnsurePass(dev, render_width, render_height)) return false;
  auto* const native = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
  const bool game_exposure = g_config.game_exposure;
  if (g_dlss.has_feature() &&
      (g_dlss.feature_width() != width || g_dlss.feature_height() != height ||
       !g_dlss.accepts(render_width, render_height) || std::fabs(g_dlss.feature_scale() - scale) > 0.01f ||
       g_dlss.feature_preset() != g_config.preset || g_dlss.feature_auto_exposure() == game_exposure)) {
    g_recreate = true;  // the old feature may still be in flight; it is released at the present once the GPU is idle
    return false;
  }
  if (!g_dlss.has_feature()) {
    const bool created = g_dlss.Create(native, width, height, render_width, render_height, scale, g_config.preset,
                                       true, !game_exposure);
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
  cmd_list->dispatch((render_width + 7) / 8, (render_height + 7) / 8, 1);
  cmd_list->barrier(2, ours, writable, readable);

  const uint64_t frame = g_frame.load();
  p3rdlss::DlssInputs in;
  in.color = reinterpret_cast<ID3D12Resource*>(color.handle);
  in.depth = reinterpret_cast<ID3D12Resource*>(depth.handle);
  in.motion = reinterpret_cast<ID3D12Resource*>(g_motion.handle);
  in.output = reinterpret_cast<ID3D12Resource*>(output.handle);
  in.exposure = game_exposure ? reinterpret_cast<ID3D12Resource*>(g_exposure.handle) : nullptr;
  in.width = render_width;
  in.height = render_height;
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
  if (g_config.dev_dumps && g_dev_frames_left.load() > 0) {
    const int32_t* const samples = g_taa_samples.load();
    char json[448];
    std::snprintf(json, sizeof(json),
                  "{\"frame\": %llu, \"dispatch\": %llu, \"render\": [%u, %u], \"input\": [%u, %u], \"output\": [%u, %u], "
                  "\"jitter\": [%.6f, %.6f], \"pre_exposure\": %.6g, \"phases\": %d, \"reset\": %d, \"mode\": %d, "
                  "\"preset\": %d, \"taa_params\": [%.0f, %.0f, %.4f, %.4f], \"ssao_off\": %d}",
                  static_cast<unsigned long long>(frame), static_cast<unsigned long long>(dispatch), render_width,
                  render_height, in_width, in_height, width, height, jitter_x, jitter_y, c.pre_exposure,
                  samples != nullptr ? samples[1] : -1, in.reset ? 1 : 0, g_config.mode, g_config.preset,
                  c.taa_params[0], c.taa_params[1], c.taa_params[2], c.taa_params[3], g_dev_no_ssao.load() ? 1 : 0);
    DevDumpFrame(cmd_list, color, render_width, render_height, output, width, height, json);
  }
  g_last_dlss_frame = frame;
  g_render_width = render_width;
  g_render_height = render_height;
  g_last_jitter_x = jitter_x;
  g_last_jitter_y = jitter_y;
  if (std::fabs(jitter_x + 0.5f) < 1e-3f && std::fabs(jitter_y + 0.5f) < 1e-3f) ++g_corner_jitter_frames;
  const uint64_t frames = ++g_dlss_frames;
  if (frames == 1 || frames % 3000 == 0) {
    const int32_t* const samples = g_taa_samples.load();
    char text[320];
    std::snprintf(text, sizeof(text),
                  "DLSS frame %llu (TAA dispatch %llu): %ux%u to %ux%u, jitter %+.3f %+.3f px, %d jitter phases, "
                  "corner jitter frames %llu, TAA sample index %.0f of %.0f, pre-exposure %.3g, camera cut %d, "
                  "reset %d, failures %llu",
                  static_cast<unsigned long long>(frames), static_cast<unsigned long long>(dispatch), render_width,
                  render_height, width, height, jitter_x, jitter_y, samples != nullptr ? samples[1] : -1,
                  static_cast<unsigned long long>(g_corner_jitter_frames.load()), c.taa_params[0], c.taa_params[1],
                  c.pre_exposure,
                  c.camera_cut ? 1 : 0, in.reset ? 1 : 0, static_cast<unsigned long long>(g_failures.load()));
    Log(reshade::log::level::info, text);
  }
  return true;
}

// --- Development: TAA-like passes in the log (RenderScale > 0) -------------------------------------------------

// The root constant buffers bound at b0 and b1 for the compute root signature `layout`, as ReadTaaConstants finds them.
bool RootConstantBuffers(const p3rdlss::Descriptors& descriptors, const CmdData& data, pipeline_layout layout,
                         buffer_range* b0, buffer_range* b1) {
  *b0 = {};
  *b1 = {};
  const std::vector<p3rdlss::Descriptors::ParamInfo> params = descriptors.Params(layout);
  for (uint32_t i = 0; i < params.size(); ++i) {
    const p3rdlss::Descriptors::ParamInfo& p = params[i];
    if ((p.type != pipeline_layout_param_type::push_descriptors &&
         p.type != pipeline_layout_param_type::push_descriptors_with_ranges &&
         p.type != pipeline_layout_param_type::push_descriptors_with_ranges_and_flags) ||
        p.ranges.size() != 1 || p.ranges[0].type != descriptor_type::constant_buffer ||
        p.ranges[0].dx_register_space != 0 || data.cbv_layout != layout || i >= data.cbvs.size())
      continue;
    if (p.ranges[0].dx_register_index == kGlobalsCbv) *b0 = data.cbvs[i];
    if (p.ranges[0].dx_register_index == kViewCbv) *b1 = data.cbvs[i];
  }
  return b0->buffer != 0 && b1->buffer != 0;
}

// `count` 32-bit values of a constant buffer from byte `first` on, four to a row: floats where they look like floats,
// otherwise the integer ("u" + value).
std::string ConstantRows(device* dev, const buffer_range& range, uint32_t first, uint32_t count) {
  std::vector<uint32_t> values(count);
  if (!ReadBuffer(dev, range, first, count * 4, values.data())) return " (not readable)";
  std::string out;
  char text[48];
  for (uint32_t i = 0; i < count; ++i) {
    if (i % 4 == 0) {
      std::snprintf(text, sizeof(text), "%s+%u:", i == 0 ? "" : " |", first + 4 * i);
      out += text;
    }
    float f;
    std::memcpy(&f, &values[i], 4);
    if (values[i] == 0 || (std::isfinite(f) && std::fabs(f) >= 1e-6f && std::fabs(f) < 1e8f))
      std::snprintf(text, sizeof(text), " %.7g", f);
    else
      std::snprintf(text, sizeof(text), " u%u", values[i]);
    out += text;
  }
  return out;
}

// A TAA-like pass (the velocity and depth-stencil inputs of TAAStandalone) in the log, once per shader and input size:
// its digest, dispatch size, inputs and output, and both constant buffers ($Globals whole, View around the values the
// add-on reads), for a permutation the add-on does not know yet, such as the one for a render scale below 100%.
void LogTaaPass(command_list* cmd_list, const CmdData& data, uint32_t gx, uint32_t gy, uint32_t gz) {
  device* const dev = cmd_list->get_device();
  auto* const state = cmd_list->get_private_data<state_tracking>();
  auto* const descriptors = dev->get_private_data<p3rdlss::Descriptors>();
  if (state == nullptr || descriptors == nullptr) return;
  const Tables* const tables = ComputeTables(*state);
  if (tables == nullptr || tables->first == 0) return;
  const auto bound = [&](RegisterKind kind, uint32_t reg) {
    return BoundResource(dev, *descriptors, *tables, kind, reg);
  };
  const resource velocity = bound(RegisterKind::kShaderResource, kVelocitySrv);
  const resource depth = bound(RegisterKind::kShaderResource, kDepthSrv);
  if (velocity == 0 || depth == 0) return;
  const resource_desc vd = dev->get_resource_desc(velocity), dd = dev->get_resource_desc(depth);
  if (vd.type != resource_type::texture_2d || vd.texture.width <= 1 ||
      format_to_typeless(vd.texture.format) != format::r16g16b16a16_typeless ||
      format_to_typeless(dd.texture.format) != format::r32_g8_typeless)
    return;

  Digest digest = {};
  {
    std::shared_lock lock(g_taa_mutex);
    const auto it = g_compute_digests.find(data.current.handle);
    if (it != g_compute_digests.end()) digest = it->second;
  }
  const resource color = bound(RegisterKind::kShaderResource, kSceneColorSrv);
  const resource output = bound(RegisterKind::kUnorderedAccess, kOutputUav);
  const std::string key = Hex(digest) + " " + Describe(dev, color) + " " + Describe(dev, output);
  {
    std::lock_guard lock(g_logged_passes_mutex);
    if (g_logged_passes.size() >= 64 || !g_logged_passes.insert(key).second) return;
  }
  const int permutation = FindTaaPermutation(digest);
  char text[200];
  std::snprintf(text, sizeof(text), "TAA-like pass: shader %s (%s), dispatch %ux%ux%u.", Hex(digest).c_str(),
                permutation != kNotTaa ? kTaaPermutations[permutation].name : "not a known permutation", gx, gy, gz);
  Log(reshade::log::level::info, text);
  std::string inputs;
  for (uint32_t t = 0; t <= 5; ++t)
    inputs += (t == 0 ? "  t" : ", t") + std::to_string(t) + " " + Describe(dev, bound(RegisterKind::kShaderResource, t));
  Log(reshade::log::level::info, inputs + ", u0 " + Describe(dev, output));
  buffer_range b0, b1;
  if (!RootConstantBuffers(*descriptors, data, tables->first, &b0, &b1)) {
    Log(reshade::log::level::info, "  (its root constant buffers b0 and b1 were not seen)");
    return;
  }
  const resource_desc b0d = dev->get_resource_desc(b0.buffer);
  const uint64_t b0_left = b0d.buffer.size > b0.offset ? b0d.buffer.size - b0.offset : 0;
  const auto b0_values = static_cast<uint32_t>((std::min)(b0_left, uint64_t{512}) / 4);
  Log(reshade::log::level::info, "  b0 $Globals" + ConstantRows(dev, b0, 0, b0_values));
  Log(reshade::log::level::info, "  b1 View" + ConstantRows(dev, b1, kClipToPrevClip, (2320 - kClipToPrevClip) / 4));
}

bool OnDispatch(command_list* cmd_list, uint32_t gx, uint32_t gy, uint32_t gz) {
  auto* const data = cmd_list->get_private_data<CmdData>();
  if (data == nullptr) return false;
  if (g_config.dev_dumps) DevTrace(cmd_list, true, gx, gy, gz);
  if (data->taa_like) LogTaaPass(cmd_list, *data, gx, gy, gz);
  if (data->taa == kNotTaa) return false;
  if (!ReplaceTaa(cmd_list, *data)) return false;
  if (g_last_permutation.exchange(data->taa) != data->taa)
    Log(reshade::log::level::info,
        std::string("DLSS replaces the TAA pass of ") + kTaaPermutations[data->taa].name + ".");
  return true;
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
  int32_t* upsampling = p3rdlss::FindIntConsoleVariable(L"r.TemporalAA.Upsampling", &g_upsampling_detail);
  // A switch: both copies 0 or 1 (the engine's default and the game's are 0).
  const auto is_switch = [](int32_t v) { return v == 0 || v == 1; };
  if (upsampling != nullptr && !(is_switch(upsampling[0]) && is_switch(upsampling[1]))) {
    g_upsampling_detail += ", but its values " + std::to_string(upsampling[0]) + " and " +
                           std::to_string(upsampling[1]) + " are no switch";
    upsampling = nullptr;
  }
  if (upsampling != nullptr) {
    g_upsampling_game[0] = upsampling[0];
    g_upsampling_game[1] = upsampling[1];
    g_taa_upsampling = upsampling;
    Log(reshade::log::level::info, "r.TemporalAA.Upsampling " + g_upsampling_detail + "; the game's value is " +
                                       std::to_string(upsampling[0]) + ".");
  } else {
    Log(reshade::log::level::warning, "r.TemporalAA.Upsampling not found (" + g_upsampling_detail +
                                          "); DLSS runs on the game's TAA path and its jitter pattern.");
  }
  float* bias = p3rdlss::FindFloatConsoleVariable(L"r.ViewTextureMipBias.Offset", &g_mip_bias_detail);
  // A small number of mip levels (the engine's default is -0.3).
  const auto is_bias = [](float v) { return std::isfinite(v) && v >= -8.0f && v <= 8.0f; };
  if (bias != nullptr && !(is_bias(bias[0]) && is_bias(bias[1]))) {
    g_mip_bias_detail += ", but its values are no mip bias";
    bias = nullptr;
  }
  if (bias != nullptr) {
    g_mip_bias_game[0] = bias[0];
    g_mip_bias_game[1] = bias[1];
    g_mip_bias_offset = bias;
    char text[64];
    std::snprintf(text, sizeof(text), "; the game's value is %.2f.", bias[0]);
    Log(reshade::log::level::info, "r.ViewTextureMipBias.Offset " + g_mip_bias_detail + text);
  } else {
    Log(reshade::log::level::warning, "r.ViewTextureMipBias.Offset not found (" + g_mip_bias_detail +
                                          "); the upsampling path keeps the engine's texture mip bias.");
  }
  // The game's frame rate limit: t.MaxFPS (a frame rate, 0 for none) and rhi.SyncInterval (0 to 4).
  float* max_fps = p3rdlss::FindFloatConsoleVariable(L"t.MaxFPS", &g_max_fps.detail);
  const auto is_fps = [](float v) { return std::isfinite(v) && v >= 0.0f && v <= 10000.0f; };
  if (max_fps != nullptr && !(is_fps(max_fps[0]) && is_fps(max_fps[1]))) {
    g_max_fps.detail += ", but its values are no frame rates";
    max_fps = nullptr;
  }
  int32_t* sync = p3rdlss::FindIntConsoleVariable(L"rhi.SyncInterval", &g_sync_interval.detail);
  const auto is_interval = [](int32_t v) { return v >= 0 && v <= 4; };
  if (sync != nullptr && !(is_interval(sync[0]) && is_interval(sync[1]))) {
    g_sync_interval.detail += ", but its values are no sync intervals";
    sync = nullptr;
  }
  {
    char text[160];
    std::snprintf(text, sizeof(text), "The game's frame rate limit: t.MaxFPS %s, rhi.SyncInterval %s.",
                  max_fps != nullptr ? std::to_string(static_cast<int>(max_fps[0])).c_str() : "not found",
                  sync != nullptr ? std::to_string(sync[0]).c_str() : "not found");
    Log(reshade::log::level::info, text);
    if (max_fps == nullptr) Log(reshade::log::level::warning, "t.MaxFPS: " + g_max_fps.detail);
    if (sync == nullptr) Log(reshade::log::level::warning, "rhi.SyncInterval: " + g_sync_interval.detail);
  }
  if (max_fps != nullptr) g_max_fps.Found(max_fps);
  if (sync != nullptr) g_sync_interval.Found(sync);
  if (int32_t* const ao = p3rdlss::FindIntConsoleVariable(L"r.AmbientOcclusionLevels", &g_ao_levels.detail))
    g_ao_levels.Found(ao);
  {
    float* percent = p3rdlss::FindFloatConsoleVariable(L"r.ScreenPercentage", &g_screen_percentage.detail);
    const auto is_percent = [](float v) { return std::isfinite(v) && v >= 1.0f && v <= 400.0f; };
    if (percent != nullptr && !(is_percent(percent[0]) && is_percent(percent[1]))) {
      g_screen_percentage.detail += ", but its values are no percentages";
      percent = nullptr;
    }
    if (percent != nullptr) {
      g_screen_percentage.Found(percent);
      char text[160];
      std::snprintf(text, sizeof(text), "r.ScreenPercentage: the game's value is %.0f; %.1f while DLSS runs.",
                    percent[0], RenderScale() * 100.0f);
      Log(reshade::log::level::info, text);
    } else {
      Log(reshade::log::level::warning, "r.ScreenPercentage not found (" + g_screen_percentage.detail +
                                            "); DLSS runs as DLAA only.");
    }
  }
  if (g_reflex.Init(device)) {
    g_reflex_ready = true;
    Log(reshade::log::level::info, "Reflex is available.");
  } else {
    Log(reshade::log::level::warning, "Reflex unavailable: " + g_reflex.status());
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
  if (int32_t* const upsampling = g_taa_upsampling.exchange(nullptr)) {
    upsampling[0] = g_upsampling_game[0];
    upsampling[1] = g_upsampling_game[1];
  }
  g_max_fps.Restore();
  g_sync_interval.Restore();
  g_screen_percentage.Restore();
  g_ao_levels.Restore();
  if (g_reflex_ready.exchange(false)) g_reflex.Configure(false, 0);
  if (float* const bias = g_mip_bias_offset.exchange(nullptr)) {
    bias[0] = g_mip_bias_game[0];
    bias[1] = g_mip_bias_game[1];
  }
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

// The engine's settings DLSS wants while it runs, the game's otherwise.
void ApplyEngineVariables() {
  g_ao_levels.Apply(g_dev_no_ssao.load(), 0);
  const bool dlss = g_config.enabled && g_dlss_available;
  // The jitter phases. The game's own changes of the variable land in [0] first, so [0] keeps its value; the jitter
  // reads [1]. Below 100% the upsampling path multiplies them by 1 / scale^2, and this game keeps the sample index in
  // a signed byte (movsx from the view state, docs/recon.md): above 128 phases it turns negative, Halton gives 0 and
  // the jitter sits at the pixel's corner (-0.5, -0.5) for 127 frames of every 256 (seen at Ultra Performance:
  // 16 x 9 = 143 phases after the engine's rounding; the image comes and goes). So the product stays at 127 or less
  // (Ultra Performance 14 x 9 = 125, above NVIDIA's 72).
  if (int32_t* const samples = g_taa_samples.load()) {
    int32_t want = dlss && g_config.jitter_phases > 0 ? g_config.jitter_phases : samples[0];
    const float render_scale = RenderScale();
    if (dlss && g_config.upsampling_path && g_taa_upsampling.load() != nullptr && render_scale < 1.0f)
      want = (std::min)(want, (std::max)(1, static_cast<int32_t>(127.0f * render_scale * render_scale)));
    if (samples[1] != want) samples[1] = want;
  }
  // The temporal upsampling path and its uniform Halton jitter. Both copies are set, as which thread reads the
  // variable is not known; a switch changes the jitter sequence, so DLSS starts its history over.
  if (int32_t* const upsampling = g_taa_upsampling.load()) {
    const bool ours = dlss && g_config.upsampling_path;
    bool changed = false;
    for (int i = 0; i < 2; ++i) {
      const int32_t want = ours ? 1 : g_upsampling_game[i];
      if (upsampling[i] != want) {
        upsampling[i] = want;
        changed = true;
      }
    }
    if (changed) g_reset = true;
  }
  // The texture mip bias, which the engine applies on the upsampling path only.
  if (float* const bias = g_mip_bias_offset.load()) {
    const bool ours = dlss && g_config.upsampling_path && g_taa_upsampling.load() != nullptr;
    for (int i = 0; i < 2; ++i) {
      const float want = ours ? g_config.mip_bias : g_mip_bias_game[i];
      if (bias[i] != want) bias[i] = want;
    }
  }
  // Reflex and the frame rate cap, whether DLSS runs or not. With a cap the game presents on every refresh instead of
  // every other one, and Reflex's sleep holds the cap (the engine's t.MaxFPS stays off then, so that only one limiter
  // waits); without Reflex t.MaxFPS does.
  const float cap = g_config.frame_rate_cap;
  const bool capping = cap > 0.0f;
  const bool reflex_on = g_config.reflex && g_reflex_ready;
  const bool reflex_caps = capping && reflex_on;
  const bool sync_ours = capping && (g_sync_interval.game[0] > 1 || g_sync_interval.game[1] > 1);
  if (g_sync_interval.Apply(sync_ours, 1))
    Log(reshade::log::level::info, "The game set rhi.SyncInterval to " + std::to_string(g_sync_interval.game[0]) + ".");
  if (g_max_fps.Apply(capping, reflex_caps ? 0.0f : cap))
    Log(reshade::log::level::info,
        "The game set t.MaxFPS to " + std::to_string(static_cast<int>(g_max_fps.game[0])) + ".");
  if (g_reflex_ready) g_reflex.Configure(reflex_on, reflex_caps ? static_cast<uint32_t>(1e6f / cap) : 0u);
  // The quality mode's render scale (r.ScreenPercentage), on the upsampling path only: the other path cannot upscale.
  const float render_scale = RenderScale();
  const bool scaling = dlss && g_config.upsampling_path && g_taa_upsampling.load() != nullptr && render_scale < 1.0f;
  if (g_screen_percentage.Apply(scaling, render_scale * 100.0f))
    Log(reshade::log::level::info,
        "The game set r.ScreenPercentage to " + std::to_string(static_cast<int>(g_screen_percentage.game[0])) + ".");
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
  ApplyEngineVariables();

  if (g_recreate.load() && g_nvidia_ready) {
    queue->wait_idle();
    std::lock_guard lock(g_dlss_mutex);
    g_dlss.ReleaseFeature();
    g_recreate = false;
  }
  if (g_config.dev_dumps) DevDumpPresent(queue, frame);
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
  if (ImGui::Checkbox("Use DLSS instead of the game's TAA", &enabled)) {
    SetDlssEnabled(enabled);
    SaveConfig();
  }
  const std::string toggle = "Toggle in game with " + KeyName(g_config.toggle_key) +
                             " (ToggleKey in Persona3ReloadDLSS.ini). Off renders at the game's own resolution.";
  ImGui::SetItemTooltip("%s", toggle.c_str());

  const char* mode_names[kModeCount];
  for (int i = 0; i < kModeCount; ++i) mode_names[i] = kModes[i].name;
  ImGui::BeginDisabled(!g_config.upsampling_path || g_config.render_scale > 0.0f);
  if (ImGui::Combo("Mode", &g_config.mode, mode_names, kModeCount)) SaveConfig();
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("The resolution the game renders at while DLSS runs; DLSS turns it into the output "
                        "resolution. DLAA renders at full resolution for the best image; the others trade image "
                        "quality for frame rate. Needs the Halton jitter path below; RenderScale in the ini "
                        "overrides it.");

  // NVIDIA's default depends on the mode (DLSS 310: K for DLAA, Quality and Balanced, M for Performance, L for Ultra
  // Performance); the list names the one the current mode gets.
  const std::string default_name = std::string("NVIDIA's default for this mode (") +
                                   p3rdlss::DefaultPresetLetter(RenderScale()) + ")";
  const char* const preset_names[] = {default_name.c_str(), "E (older CNN model, lightest)", "J", "K", "L", "M"};
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
  ImGui::SetItemTooltip("NVIDIA's default picks per mode: K (transformer) for DLAA, Quality and Balanced, M for "
                        "Performance, L for Ultra Performance.\nA letter here applies to every mode. L and M cost more "
                        "than K (much more on RTX 20 and 30 cards); E is the older CNN model, about half of K's cost.");

  if (ImGui::SliderInt("Jitter phases", &g_config.jitter_phases, 0, 32)) g_reset = true;
  if (ImGui::IsItemDeactivatedAfterEdit()) SaveConfig();
  ImGui::SetItemTooltip("While DLSS runs, the camera jitter cycles through this many sub-pixel positions "
                        "(r.TemporalAASamples). The game uses 2, which leaves DLSS too little to work with; NVIDIA "
                        "asks for at least 8. 0 keeps the game's.\nBelow 100%% the engine multiplies them by "
                        "1 / scale^2, and this game's jitter breaks above 128 phases, so Ultra Performance gets at "
                        "most 14 (x 9).");
  if (ImGui::Checkbox("Halton jitter (the engine's upsampling path)", &g_config.upsampling_path)) SaveConfig();
  ImGui::SetItemTooltip("While DLSS runs, the engine renders through its temporal upsampling path at 100%% "
                        "(r.TemporalAA.Upsampling 1): the camera jitter covers the whole pixel in a Halton sequence, "
                        "the pattern NVIDIA trains DLSS on. The game's own TAA path bends it into a disc that never "
                        "samples the pixel's corners. This path also applies the texture mip bias below.");
  ImGui::BeginDisabled(!g_config.upsampling_path);
  ImGui::SliderFloat("Texture mip bias", &g_config.mip_bias, -2.0f, 0.0f, "%.1f");
  if (ImGui::IsItemDeactivatedAfterEdit()) SaveConfig();
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("Sharper (more negative) or softer texture sampling while DLSS runs on the upsampling path "
                        "(r.ViewTextureMipBias.Offset). NVIDIA recommends -1.0 for DLAA. If fine, high-contrast "
                        "textures shimmer or show moire in motion, move it toward 0. The game's own TAA path uses 0.");
  if (ImGui::Checkbox("Game exposure", &g_config.game_exposure)) SaveConfig();
  ImGui::SetItemTooltip("Give DLSS the game's eye adaptation exposure. Off lets DLSS estimate one.");
  if (ImGui::Checkbox("Smooth SSAO noise", &g_config.ssao_noise_fix)) SaveConfig();
  ImGui::SetItemTooltip("The game's ambient occlusion steps its 4x4 noise pattern in time with the camera jitter, so "
                        "DLSS keeps it as a fine grid (on skin, in corners). On: while DLSS runs, the noise takes a "
                        "random one of its 8 steps each frame and DLSS averages it out, as the game's TAA does.");

  ImGui::Separator();
  if (ImGui::Checkbox("NVIDIA Reflex low latency", &g_config.reflex)) SaveConfig();
  ImGui::SetItemTooltip("When the GPU is the limit, the game's CPU side runs ahead and queues finished frames, each "
                        "adding a frame of input latency. Reflex holds the CPU back so every frame starts just in "
                        "time. Injected at the present, as the game has no Reflex of its own.");
  float cap = g_config.frame_rate_cap;
  if (ImGui::InputFloat("Frame rate cap", &cap, 1.0f, 10.0f, "%.0f")) {
    g_config.frame_rate_cap = cap <= 0.0f ? 0.0f : std::clamp(cap, 30.0f, 1000.0f);
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) SaveConfig();
  ImGui::SetItemTooltip("0 keeps the game's limit (its frame rate option). With a cap the game presents on every "
                        "refresh instead of every other one, and Reflex's sleep holds the cap (the engine's t.MaxFPS "
                        "without Reflex). With a variable refresh rate display, a few frames under its refresh rate, "
                        "for example 237 at 240 Hz.");

  ImGui::Separator();
  ImGui::TextWrapped("DLSS: %s", DlssStatus().c_str());
  {
    std::lock_guard lock(g_info_mutex);
    ImGui::TextWrapped("%s", g_taa_info.c_str());
  }
  if (g_nvidia_ready) {
    if (const int32_t* const samples = g_taa_samples.load()) {
      // Below 100% the upsampling path multiplies the phases by 1 / scale^2 (in float, truncated), as the engine does.
      const uint32_t render_width = g_render_width.load(), out_width = g_out_width.load();
      const int32_t* const upsampling = g_taa_upsampling.load();
      if (upsampling != nullptr && upsampling[1] != 0 && render_width != 0 && render_width < out_width) {
        const float fraction = static_cast<float>(render_width) / static_cast<float>(out_width);
        ImGui::Text("Camera jitter: %d phases, %d at this render scale (the game asks for %d)", samples[1],
                    static_cast<int>(static_cast<float>(samples[1]) / (fraction * fraction)), samples[0]);
      } else {
        ImGui::Text("Camera jitter: %d phases (the game asks for %d)", samples[1], samples[0]);
      }
    } else
      ImGui::TextWrapped("Camera jitter: the game's (r.TemporalAASamples not found: %s)", g_jitter_detail.c_str());
    if (const int32_t* const upsampling = g_taa_upsampling.load())
      ImGui::Text("Temporal upsampling path: %s", upsampling[1] != 0 ? "on" : "off");
    else
      ImGui::TextWrapped("Temporal upsampling path: not available (r.TemporalAA.Upsampling not found: %s)",
                         g_upsampling_detail.c_str());
    if (const float* const bias = g_mip_bias_offset.load())
      ImGui::Text("Texture mip bias on that path: %.2f (the engine's default %.2f)", bias[1], g_mip_bias_game[1]);
    else
      ImGui::TextWrapped("Texture mip bias: the engine's (r.ViewTextureMipBias.Offset not found: %s)",
                         g_mip_bias_detail.c_str());
    const int permutation = g_last_permutation.load();
    if (permutation != kNotTaa)
      ImGui::Text("DLSS last replaced the TAA pass of %s.", kTaaPermutations[permutation].name);
    ImGui::TextWrapped("Reflex: %s", g_reflex_ready ? g_reflex.SleepStatus().c_str() : g_reflex.status().c_str());
    if (const float* const percent = g_screen_percentage.values.load())
      ImGui::Text("Render scale: %.1f%% (the game's %.0f%%)", percent[0], g_screen_percentage.game[0]);
    else
      ImGui::TextWrapped("Render scale: the game's (r.ScreenPercentage not found: %s)",
                         g_screen_percentage.detail.c_str());
    const float* const max_fps = g_max_fps.values.load();
    const int32_t* const sync = g_sync_interval.values.load();
    if (max_fps != nullptr && sync != nullptr)
      ImGui::Text("Frame rate limit: t.MaxFPS %.0f, sync interval %d (the game's: %.0f and %d)", max_fps[0], sync[1],
                  g_max_fps.game[0], g_sync_interval.game[1]);
    else
      ImGui::TextWrapped("Frame rate limit: the game's (t.MaxFPS %s, rhi.SyncInterval %s)", g_max_fps.detail.c_str(),
                         g_sync_interval.detail.c_str());
  }
  const uint64_t last = g_last_dlss_frame.load();
  if (g_dlss_frames.load() != 0 && last + 2 >= g_frame.load())
    ImGui::Text("Running: %ux%u to %ux%u, jitter %+.2f %+.2f px", g_render_width.load(), g_render_height.load(),
                g_out_width.load(), g_out_height.load(), g_last_jitter_x.load(), g_last_jitter_y.load());
  else if (g_config.enabled)
    ImGui::TextUnformatted("Not running this frame (the game's TAA runs).");
  if (g_failures.load() != 0) ImGui::Text("Failed evaluations: %llu", static_cast<unsigned long long>(g_failures.load()));
  if (g_config.ssao_noise_fix) {
    if (g_ao_last_frame.load() + 2 >= g_frame.load())
      ImGui::Text("SSAO noise: step %d (the game's: %d), %u constant buffer row(s)", g_ao_to.load(), g_ao_from.load(),
                  g_ao_rows.load());
    else
      ImGui::TextUnformatted("SSAO noise: the SSAO pass was not seen this frame");
  }
  if (g_config.dev_dumps) {
    ImGui::Separator();
    bool no_ssao = g_dev_no_ssao.load();
    if (ImGui::Checkbox("Development: SSAO off (r.AmbientOcclusionLevels 0)", &no_ssao)) {
      g_dev_no_ssao = no_ssao;
      Log(reshade::log::level::info, no_ssao ? "Development: SSAO off." : "Development: SSAO back to the game's.");
    }
    if (g_ao_levels.values.load() != nullptr)
      ImGui::Text("r.AmbientOcclusionLevels: the game's %d, now %d", g_ao_levels.game[1], g_ao_levels.values.load()[1]);
    else
      ImGui::TextWrapped("r.AmbientOcclusionLevels not found: %s", g_ao_levels.detail.c_str());
  }
  if (g_corner_jitter_frames.load() != 0)
    ImGui::TextWrapped("Jitter stuck at the pixel's corner in %llu frames (the engine's sample index wrapped: more "
                       "than 128 phases).",
                       static_cast<unsigned long long>(g_corner_jitter_frames.load()));
}

void Initialize(HMODULE module) {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(module, path, MAX_PATH);
  g_addon_dir = fs::path(path).parent_path();
  g_data_dir = g_addon_dir / L"Persona3ReloadDLSS";
  g_ini_path = g_addon_dir / L"Persona3ReloadDLSS.ini";
  LoadConfig();
  g_diagnostics = g_config.log_taa_passes;
  if (g_diagnostics) Log(reshade::log::level::info, "Development: TAA-like passes are logged (LogTaaPasses).");
  if (g_config.dev_dumps) {
    g_dev_dir = g_data_dir / L"dev";
    std::error_code error;
    fs::create_directories(g_dev_dir / L"shaders", error);
    fs::create_directories(g_dev_dir / L"frames", error);
    Log(reshade::log::level::info, "Development: shaders go to Persona3ReloadDLSS\\dev\\shaders, and a file "
                                   "dev\\dump.txt dumps the next DLSS frames (DevDumps).");
  }
  if (g_config.render_scale > 0.0f) {
    char scale[128];
    std::snprintf(scale, sizeof(scale), "Development: render scale %.2f in place of the mode's (RenderScale).",
                  g_config.render_scale);
    Log(reshade::log::level::info, scale);
  }
  char text[256];
  std::snprintf(text, sizeof(text),
                "Loaded version " PROJECT_VERSION_STRING
                ". DLSS %s, %s; toggle key %s; %d jitter phases; upsampling path %s, mip bias %.2f; preset %d.",
                g_config.enabled ? "on" : "off", kModes[g_config.mode].name, KeyName(g_config.toggle_key).c_str(),
                g_config.jitter_phases, g_config.upsampling_path ? "on" : "off", g_config.mip_bias, g_config.preset);
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
      reshade::register_event<reshade::addon_event::draw>(OnDevDraw);
      reshade::register_event<reshade::addon_event::draw_indexed>(OnDevDrawIndexed);
      reshade::register_event<reshade::addon_event::destroy_resource>(OnDestroyResource);
      reshade::register_event<reshade::addon_event::present>(OnPresent);
      // Reflex: the present markers after every other present callback, right before the swap chain presents; the
      // sleep right after it; the frame's first submission.
      reshade::register_event<reshade::addon_event::present>(
          [](command_queue*, swapchain*, const rect*, const rect*, uint32_t, const rect*) {
            if (g_reflex_ready) g_reflex.BeforePresent();
          });
      reshade::register_event<reshade::addon_event::finish_present>([](command_queue*, swapchain*) {
        if (g_reflex_ready) g_reflex.AfterPresent();
      });
      reshade::register_event<reshade::addon_event::execute_command_list>([](command_queue*, command_list*) {
        if (g_reflex_ready) g_reflex.OnSubmit();
      });
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
