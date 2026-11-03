// NVIDIA Reflex low-latency mode, injected at the present (from EldenRingDLSS). When the GPU is the bottleneck the
// game's CPU side runs ahead and queues finished frames, each adding a frame of input latency; Reflex's sleep holds
// the CPU back so every frame starts just in time. A game with built-in Reflex sleeps right before it samples input;
// the add-on only sees the graphics API, so it sleeps right after each present (ReShade's finish_present), which is
// the start of the next frame as far as the driver can tell, and marks the present for the driver's latency tracking.
// The same sleep can also hold a frame rate cap (minimumIntervalUs), which NVIDIA recommends over a game's own
// limiter: the frame then waits before it starts, not after it is finished.
// SPDX-License-Identifier: MIT

#pragma once

#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace p3rdlss {

class Reflex {
 public:
  // Loads NVAPI for `device` (an NVIDIA GPU with a Reflex-capable driver). False otherwise; the add-on then runs
  // without it.
  bool Init(ID3D12Device* device);
  bool ready() const { return device_ != nullptr; }
  const std::string& status() const { return status_; }

  // Low-latency mode on or off and the frame rate cap as the shortest frame interval (0: none); the driver keeps the
  // setting for the device. Calls the driver only when something changed.
  bool Configure(bool on, uint32_t minimum_interval_us);
  bool enabled() const { return enabled_.load(); }
  uint32_t minimum_interval_us() const { return interval_us_.load(); }

  // Around the swap chain's Present, on the presenting thread: the present markers, then the sleep and the next
  // frame's simulation start.
  void BeforePresent();
  void AfterPresent();
  // At every ExecuteCommandLists: the frame's first submission is its render-submit start.
  void OnSubmit();

  // The driver's view: low-latency mode, whether it sees our sleep, the last sleep interval, fullscreen VRR.
  std::string SleepStatus();
  // Medians over the driver's last 64 frame reports (NvAPI_D3D_GetLatency), and how long the sleeps blocked since
  // the last call.
  std::string Timeline();

 private:
  bool active() const { return device_ != nullptr && enabled_.load(); }
  void Marker(int type);

  std::atomic<ID3D12Device*> device_{nullptr};  // set by Init, on another thread than the presents
  std::string status_ = "off";
  std::atomic<bool> enabled_{false};
  std::atomic<uint32_t> interval_us_{0};
  bool configured_ = false;
  std::atomic<uint64_t> frame_{1};
  std::atomic<uint64_t> sleeps_{0};
  std::atomic<bool> submit_marked_{false};
  std::atomic<uint64_t> sleep_us_{0}, sleep_max_us_{0}, sleep_count_{0};  // since the last Timeline()
  std::mutex mutex_;
};

}  // namespace p3rdlss
