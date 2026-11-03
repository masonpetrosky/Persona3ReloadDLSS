// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "reflex.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include <dxgi1_4.h>  // before nvapi.h, which declares functions taking IDXGISwapChain
#include <nvapi.h>

namespace p3rdlss {

bool Reflex::Init(ID3D12Device* device) {
  std::lock_guard lock(mutex_);
  if (device_ != nullptr) return true;
  const NvAPI_Status init = NvAPI_Initialize();
  if (init != NVAPI_OK) {
    status_ = "NVAPI unavailable (" + std::to_string(static_cast<int>(init)) + ")";
    return false;
  }
  NV_GET_SLEEP_STATUS_PARAMS sleep = {};
  sleep.version = NV_GET_SLEEP_STATUS_PARAMS_VER;
  const NvAPI_Status probe = NvAPI_D3D_GetSleepStatus(device, &sleep);
  if (probe != NVAPI_OK) {
    status_ = "Reflex unsupported by this GPU or driver (" + std::to_string(static_cast<int>(probe)) + ")";
    return false;
  }
  device_ = device;
  status_ = "ready";
  return true;
}

bool Reflex::Configure(bool on, uint32_t minimum_interval_us) {
  std::lock_guard lock(mutex_);
  if (device_ == nullptr) return false;
  if (configured_ && on == enabled_.load() && minimum_interval_us == interval_us_.load()) return true;
  NV_SET_SLEEP_MODE_PARAMS params = {};
  params.version = NV_SET_SLEEP_MODE_PARAMS_VER;
  params.bLowLatencyMode = on;
  params.bLowLatencyBoost = false;
  params.minimumIntervalUs = on ? minimum_interval_us : 0;
  params.bUseMarkersToOptimize = false;  // the markers are approximate: the game's own frame start is unknown
  const NvAPI_Status result = NvAPI_D3D_SetSleepMode(device_, &params);
  if (result != NVAPI_OK) {
    status_ = "SetSleepMode failed (" + std::to_string(static_cast<int>(result)) + ")";
    return false;
  }
  configured_ = true;
  enabled_ = on;
  interval_us_ = on ? minimum_interval_us : 0;
  char text[64];
  if (on && minimum_interval_us != 0)
    std::snprintf(text, sizeof(text), "on, capped at %.1f fps", 1e6 / minimum_interval_us);
  else
    std::snprintf(text, sizeof(text), "%s", on ? "on" : "off");
  status_ = text;
  return true;
}

void Reflex::Marker(int type) {
  NV_LATENCY_MARKER_PARAMS marker = {};
  marker.version = NV_LATENCY_MARKER_PARAMS_VER;
  marker.frameID = frame_.load();
  marker.markerType = static_cast<NV_LATENCY_MARKER_TYPE>(type);
  NvAPI_D3D_SetLatencyMarker(device_, &marker);
}

void Reflex::BeforePresent() {
  if (!active()) return;
  Marker(RENDERSUBMIT_END);
  Marker(PRESENT_START);
}

void Reflex::AfterPresent() {
  if (!active()) return;
  Marker(PRESENT_END);
  // The next frame starts here: the driver sleeps as long as it takes for that frame to reach an idle GPU (and, with
  // a cap, until the frame interval has passed).
  LARGE_INTEGER frequency, start, end;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&start);
  NvAPI_D3D_Sleep(device_);
  QueryPerformanceCounter(&end);
  const auto us = static_cast<uint64_t>((end.QuadPart - start.QuadPart) * 1000000 / frequency.QuadPart);
  sleep_us_ += us;
  ++sleep_count_;
  uint64_t max = sleep_max_us_.load();
  while (us > max && !sleep_max_us_.compare_exchange_weak(max, us)) {
  }
  ++sleeps_;
  frame_.fetch_add(1);
  submit_marked_ = false;
  Marker(SIMULATION_START);
}

void Reflex::OnSubmit() {
  if (!active() || submit_marked_.exchange(true)) return;
  Marker(RENDERSUBMIT_START);
}

std::string Reflex::Timeline() {
  if (device_ == nullptr) return status_;
  auto* const result = new NV_LATENCY_RESULT_PARAMS();  // 64 reports, too large for the stack
  result->version = NV_LATENCY_RESULT_PARAMS_VER;
  const NvAPI_Status status = NvAPI_D3D_GetLatency(device_, result);
  std::string out;
  char line[512];
  const uint64_t count = sleep_count_.exchange(0), total = sleep_us_.exchange(0), max = sleep_max_us_.exchange(0);
  std::snprintf(line, sizeof(line), "sleep: %llu calls, mean %.2f ms, max %.2f ms",
                static_cast<unsigned long long>(count), count ? total / 1000.0 / count : 0.0, max / 1000.0);
  out = line;
  if (status != NVAPI_OK) {
    delete result;
    return out + "; GetLatency failed (" + std::to_string(static_cast<int>(status)) + ")";
  }
  // Intervals in ms per frame (the reports' times are microseconds), medians over the frames that have them.
  struct Span {
    const char* name;
    std::vector<double> values;
  };
  Span spans[] = {{"frame start->first submit", {}}, {"first submit->present", {}}, {"present call", {}},
                  {"present->GPU start", {}},        {"GPU start->end", {}},       {"GPU active", {}},
                  {"frame start->GPU end", {}},      {"GPU frame time", {}}};
  for (const auto& f : result->frameReport) {
    if (f.frameID == 0 || f.simStartTime == 0 || f.gpuRenderEndTime == 0) continue;
    auto add = [&](int i, uint64_t from, uint64_t to) {
      if (from != 0 && to >= from) spans[i].values.push_back((to - from) / 1000.0);
    };
    add(0, f.simStartTime, f.renderSubmitStartTime);
    add(1, f.renderSubmitStartTime, f.presentStartTime);
    add(2, f.presentStartTime, f.presentEndTime);
    add(3, f.presentStartTime, f.gpuRenderStartTime);
    add(4, f.gpuRenderStartTime, f.gpuRenderEndTime);
    spans[5].values.push_back(f.gpuActiveRenderTimeUs / 1000.0);
    add(6, f.simStartTime, f.gpuRenderEndTime);
    if (f.gpuFrameTimeUs != 0) spans[7].values.push_back(f.gpuFrameTimeUs / 1000.0);
  }
  for (Span& span : spans) {
    if (span.values.empty()) {
      std::snprintf(line, sizeof(line), "; %s -", span.name);
    } else {
      std::nth_element(span.values.begin(), span.values.begin() + span.values.size() / 2, span.values.end());
      std::snprintf(line, sizeof(line), "; %s %.2f", span.name, span.values[span.values.size() / 2]);
    }
    out += line;
  }
  delete result;
  return out + " (ms, medians)";
}

std::string Reflex::SleepStatus() {
  if (device_ == nullptr) return status_;
  NV_GET_SLEEP_STATUS_PARAMS sleep = {};
  sleep.version = NV_GET_SLEEP_STATUS_PARAMS_VER;
  if (NvAPI_D3D_GetSleepStatus(device_, &sleep) != NVAPI_OK) return status_ + ", sleep status unavailable";
  char text[256];
  std::snprintf(text, sizeof(text),
                "%s; driver: low latency %s, game sleep %s, last sleep interval %u us, fullscreen VRR %s, "
                "fullscreen/iFlip %s, control panel vsync %s; %llu sleeps",
                status_.c_str(), sleep.bLowLatencyMode ? "on" : "off", sleep.bUseGameSleep ? "seen" : "not seen",
                sleep.sleepIntervalUs, sleep.bFsVrr ? "on" : "off", sleep.bFullscreenIFlip ? "yes" : "no",
                sleep.bCplVsyncOn ? "forced on" : "not forced", static_cast<unsigned long long>(sleeps_.load()));
  return text;
}

}  // namespace p3rdlss
