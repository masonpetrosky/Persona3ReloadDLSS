// Thin wrapper around NVIDIA NGX for DLSS super resolution on D3D12 (from EldenRingDLSS).
// SPDX-License-Identifier: MIT

#pragma once

#include <d3d12.h>

#include <algorithm>
#include <cstdint>
#include <string>

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace p3rdlss {

struct DlssInputs {
  // The inputs hold the render resolution in their top-left corner (all of them for DLAA).
  ID3D12Resource* color = nullptr;     // readable by compute shaders
  ID3D12Resource* depth = nullptr;
  ID3D12Resource* motion = nullptr;
  ID3D12Resource* output = nullptr;    // output resolution, in UNORDERED_ACCESS state
  ID3D12Resource* exposure = nullptr;  // 1x1 R32_FLOAT, or null when the feature uses auto-exposure
  uint32_t width = 0;                  // render resolution (the inputs' sub-rectangle)
  uint32_t height = 0;
  float jitter_x = 0.0f;               // render pixels
  float jitter_y = 0.0f;
  float mv_scale_x = 1.0f;             // converts motion vector texels to render pixels
  float mv_scale_y = 1.0f;
  float pre_exposure = 1.0f;           // the exposure the color is already multiplied by
  bool reset = false;
  float frame_time_ms = 0.0f;
};

// The preset NVIDIA's default picks for the quality mode a render scale maps to: "K", "M" or "L".
const char* DefaultPresetLetter(float scale);

class Dlss {
 public:
  // Initializes NGX for the device. nvngx_dlss.dll is loaded from `dll_dir`; NGX writes its logs to `log_dir`.
  bool Init(ID3D12Device* device, const std::wstring& dll_dir, const std::wstring& log_dir);
  void Shutdown();

  // Records feature creation on the command list for an output of `width` x `height` and render sizes up to
  // `render_width` x `render_height`. `scale` (render / output size) picks DLSS's quality mode, 1 for DLAA. `hdr`:
  // linear HDR color (before tonemapping). For HDR without `auto_exposure`, every evaluation must pass an exposure
  // texture.
  bool Create(ID3D12GraphicsCommandList* cmd, uint32_t width, uint32_t height, uint32_t render_width,
              uint32_t render_height, float scale, int preset, bool hdr, bool auto_exposure);
  // Releases the feature. The GPU must no longer use it.
  void ReleaseFeature();
  bool Evaluate(ID3D12GraphicsCommandList* cmd, const DlssInputs& inputs);

  bool available() const { return available_; }
  bool has_feature() const { return feature_ != nullptr; }
  uint32_t feature_width() const { return width_; }  // output size
  uint32_t feature_height() const { return height_; }
  float feature_scale() const { return scale_; }
  int feature_preset() const { return preset_; }
  // Whether evaluations may pass this render size.
  bool accepts(uint32_t width, uint32_t height) const {
    return width >= min_width_ && height >= min_height_ && width <= max_width_ && height <= max_height_;
  }
  bool feature_auto_exposure() const { return auto_exposure_; }
  const std::string& status() const { return status_; }

 private:
  ID3D12Device* device_ = nullptr;
  NVSDK_NGX_Parameter* params_ = nullptr;
  NVSDK_NGX_Handle* feature_ = nullptr;
  bool initialized_ = false;
  bool available_ = false;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  float scale_ = 1.0f;
  int preset_ = 0;
  uint32_t min_width_ = 0, min_height_ = 0, max_width_ = 0, max_height_ = 0;
  bool auto_exposure_ = true;
  std::string status_ = "not initialized";
};

}  // namespace p3rdlss
