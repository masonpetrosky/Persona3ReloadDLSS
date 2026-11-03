// SPDX-License-Identifier: MIT

#include "dlss.hpp"

#include <reshade.hpp>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace p3rdlss {
namespace {

// Identifies this integration to NGX (any stable GUID).
constexpr char kProjectId[] = "8e41c2d7-5a90-4f3b-b6e2-71d09a4c35f8";

std::string Hex(NVSDK_NGX_Result result) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned>(result));
  return text;
}

// DLSS's quality modes with their nominal render / output ratios and preset hints.
struct Mode {
  NVSDK_NGX_PerfQuality_Value value;
  float scale;
  const char* name;
  const char* preset_hint;
};
constexpr Mode kModes[] = {
    {NVSDK_NGX_PerfQuality_Value_DLAA, 1.0f, "DLAA", NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA},
    {NVSDK_NGX_PerfQuality_Value_UltraQuality, 0.77f, "Ultra Quality",
     NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality},
    {NVSDK_NGX_PerfQuality_Value_MaxQuality, 0.667f, "Quality", NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality},
    {NVSDK_NGX_PerfQuality_Value_Balanced, 0.58f, "Balanced", NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced},
    {NVSDK_NGX_PerfQuality_Value_MaxPerf, 0.5f, "Performance", NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance},
    {NVSDK_NGX_PerfQuality_Value_UltraPerformance, 0.333f, "Ultra Performance",
     NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance},
};

// DLAA at full size, otherwise the mode whose ratio is closest.
const Mode& PickMode(float scale) {
  if (scale >= 0.999f) return kModes[0];
  const Mode* best = &kModes[1];
  for (const Mode& mode : kModes)
    if (mode.value != NVSDK_NGX_PerfQuality_Value_DLAA &&
        std::fabs(mode.scale - scale) < std::fabs(best->scale - scale))
      best = &mode;
  return *best;
}

void NVSDK_CONV OnNgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
  std::string line = message;
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  // NGX is chatty; only pass on problems.
  if (line.find("error") != std::string::npos || line.find("Error") != std::string::npos ||
      line.find("warning") != std::string::npos || line.find("Warning") != std::string::npos)
    reshade::log::message(reshade::log::level::warning, ("[P3RDLSS] NGX: " + line).c_str());
}

}  // namespace

// The SDK's defaults (nvsdk_ngx_defs.h, 310.x): M for Performance, L for Ultra Performance, K for the other modes.
const char* DefaultPresetLetter(float scale) {
  switch (PickMode(scale).value) {
    case NVSDK_NGX_PerfQuality_Value_MaxPerf: return "M";
    case NVSDK_NGX_PerfQuality_Value_UltraPerformance: return "L";
    default: return "K";
  }
}

bool Dlss::Init(ID3D12Device* device, const std::wstring& dll_dir, const std::wstring& log_dir) {
  device_ = device;
  const wchar_t* paths[] = {dll_dir.c_str()};
  NVSDK_NGX_FeatureCommonInfo info = {};
  info.PathListInfo.Path = paths;
  info.PathListInfo.Length = 1;
  info.LoggingInfo.LoggingCallback = OnNgxLog;
  info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;

  NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
                                                                log_dir.c_str(), device, &info);
  if (NVSDK_NGX_FAILED(result)) {
    status_ = "NGX init failed " + Hex(result);
    return false;
  }
  initialized_ = true;

  NVSDK_NGX_Parameter* caps = nullptr;
  result = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
  if (NVSDK_NGX_FAILED(result) || caps == nullptr) {
    status_ = "NGX capability query failed " + Hex(result);
    return false;
  }
  int dlss_available = 0, needs_driver = 0;
  NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_Available, &dlss_available);
  NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
  NVSDK_NGX_D3D12_DestroyParameters(caps);
  if (needs_driver) {
    status_ = "DLSS needs a newer NVIDIA driver";
    return false;
  }
  if (!dlss_available) {
    status_ = "DLSS is not available on this GPU (or nvngx_dlss.dll was not found)";
    return false;
  }

  result = NVSDK_NGX_D3D12_AllocateParameters(&params_);
  if (NVSDK_NGX_FAILED(result)) {
    status_ = "NGX parameter allocation failed " + Hex(result);
    return false;
  }
  available_ = true;
  status_ = "ready";
  return true;
}

void Dlss::Shutdown() {
  ReleaseFeature();
  if (params_ != nullptr) NVSDK_NGX_D3D12_DestroyParameters(params_);
  params_ = nullptr;
  if (initialized_) NVSDK_NGX_D3D12_Shutdown1(device_);
  initialized_ = false;
  available_ = false;
  device_ = nullptr;
  status_ = "shut down";
}

bool Dlss::Create(ID3D12GraphicsCommandList* cmd, uint32_t width, uint32_t height, uint32_t render_width,
                  uint32_t render_height, float scale, int preset, bool hdr, bool auto_exposure) {
  if (!available_) return false;
  ReleaseFeature();
  for (const Mode& m : kModes) NVSDK_NGX_Parameter_SetUI(params_, m.preset_hint, static_cast<unsigned>(preset));

  // The smallest render size the mode accepts; the feature is created for the size the game renders at.
  const Mode& mode = PickMode(scale);
  uint32_t optimal_w = 0, optimal_h = 0, max_w = 0, max_h = 0, min_w = 0, min_h = 0;
  float sharpness = 0;
  NVSDK_NGX_Parameter* caps = nullptr;
  if (NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_GetCapabilityParameters(&caps)) && caps != nullptr) {
    if (NVSDK_NGX_FAILED(NGX_DLSS_GET_OPTIMAL_SETTINGS(caps, width, height, mode.value, &optimal_w, &optimal_h,
                                                       &max_w, &max_h, &min_w, &min_h, &sharpness)))
      max_w = 0;
    NVSDK_NGX_D3D12_DestroyParameters(caps);
  }
  if (max_w == 0 || max_h == 0 || min_w > max_w || min_h > max_h) min_w = min_h = 1;  // no usable answer
  max_w = (std::clamp)(render_width, (std::min)(min_w, width), width);
  max_h = (std::clamp)(render_height, (std::min)(min_h, height), height);
  min_w = (std::min)(min_w, max_w);
  min_h = (std::min)(min_h, max_h);

  // Motion vectors at render resolution without jitter, reverse-Z depth; HDR: linear color.
  NVSDK_NGX_DLSS_Create_Params create = {};
  create.Feature.InWidth = max_w;
  create.Feature.InHeight = max_h;
  create.Feature.InTargetWidth = width;
  create.Feature.InTargetHeight = height;
  create.Feature.InPerfQualityValue = mode.value;
  create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
  if (hdr) create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
  if (hdr && auto_exposure) create.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
  const NVSDK_NGX_Result result = NGX_D3D12_CREATE_DLSS_EXT(cmd, 1, 1, &feature_, params_, &create);
  if (NVSDK_NGX_FAILED(result)) {
    feature_ = nullptr;
    status_ = "DLSS feature creation failed " + Hex(result);
    return false;
  }
  width_ = width;
  height_ = height;
  scale_ = scale;
  preset_ = preset;
  min_width_ = min_w;
  min_height_ = min_h;
  max_width_ = max_w;
  max_height_ = max_h;
  auto_exposure_ = auto_exposure;
  char text[224];
  char preset_text[40];
  if (preset == 0)
    std::snprintf(preset_text, sizeof(preset_text), "NVIDIA's default, %s", DefaultPresetLetter(scale));
  else
    std::snprintf(preset_text, sizeof(preset_text), "%d", preset);
  std::snprintf(text, sizeof(text), "running %s (preset %s), render %ux%u to %ux%u, output %ux%u (%s)", mode.name,
                preset_text, min_w, min_h, max_w, max_h, width, height,
                !hdr ? "after tonemapping" : auto_exposure ? "auto-exposure" : "game exposure");
  status_ = text;
  return true;
}

void Dlss::ReleaseFeature() {
  if (feature_ != nullptr) NVSDK_NGX_D3D12_ReleaseFeature(feature_);
  feature_ = nullptr;
  width_ = height_ = 0;
  min_width_ = min_height_ = max_width_ = max_height_ = 0;
}

bool Dlss::Evaluate(ID3D12GraphicsCommandList* cmd, const DlssInputs& in) {
  if (feature_ == nullptr) return false;
  NVSDK_NGX_D3D12_DLSS_Eval_Params eval = {};
  eval.Feature.pInColor = in.color;
  eval.Feature.pInOutput = in.output;
  eval.pInDepth = in.depth;
  eval.pInMotionVectors = in.motion;
  eval.InJitterOffsetX = in.jitter_x;
  eval.InJitterOffsetY = in.jitter_y;
  eval.InRenderSubrectDimensions = {in.width, in.height};
  eval.InReset = in.reset ? 1 : 0;
  eval.InMVScaleX = in.mv_scale_x;
  eval.InMVScaleY = in.mv_scale_y;
  eval.pInExposureTexture = in.exposure;
  eval.InPreExposure = in.pre_exposure;
  eval.InExposureScale = 1.0f;
  eval.InFrameTimeDeltaInMsec = in.frame_time_ms;
  const NVSDK_NGX_Result result = NGX_D3D12_EVALUATE_DLSS_EXT(cmd, feature_, params_, &eval);
  if (NVSDK_NGX_FAILED(result)) {
    status_ = "DLSS evaluation failed " + Hex(result);
    return false;
  }
  return true;
}

}  // namespace p3rdlss
