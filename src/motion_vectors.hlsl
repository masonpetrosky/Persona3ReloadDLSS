// DLSS's motion vectors from Unreal Engine 4.27's velocity buffer, and camera motion from depth where no object
// wrote velocity, the way the engine's TAA (TAAStandalone.usf) does it; also copies the eye adaptation exposure into
// the 1x1 texture DLSS reads. docs/recon.md has the encoding and the constant buffer offsets.
// SPDX-License-Identifier: MIT

Texture2D<float4> g_Velocity : register(t0);       // R16G16B16A16_UNORM; 0 where no object wrote velocity
Texture2D<float> g_Depth : register(t1);           // device Z, reverse Z
Texture2D<float4> g_EyeAdaptation : register(t2);  // x = the exposure scale the tonemapper applies
RWTexture2D<float2> g_Motion : register(u0);       // render pixels, from this frame's position to the previous one
RWTexture2D<float> g_Exposure : register(u1);      // 1x1

cbuffer Params : register(b0) {
  float4 g_ClipToPrevClip[4];  // View.ClipToPrevClip, Unreal's row-major rows (no jitter in it)
  float4 g_ViewSizeAndInvSize;
  float2 g_ViewRectMin;
  uint g_Mode;                 // 0: velocity where written, camera motion elsewhere; 1: camera motion only; 2: zero
  uint g_Unused;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x == 0 && id.y == 0) g_Exposure[uint2(0, 0)] = g_EyeAdaptation.Load(int3(0, 0, 0)).x;
  if (any(id.xy >= uint2(g_ViewSizeAndInvSize.xy))) return;
  const int3 pixel = int3(int2(g_ViewRectMin) + int2(id.xy), 0);

  // Screen position (clip xy / w, y up) this frame minus the previous frame's, without jitter.
  float2 velocity;
  const float4 encoded = g_Velocity.Load(pixel);
  if (g_Mode == 0 && encoded.x > 0.0) {
    // EncodeVelocityToTexture: V * (0.499 * 0.5) + 32767 / 65535
    velocity = (encoded.xy - 32767.0 / 65535.0) / (0.499 * 0.5);
  } else {
    const float2 screen = ((float2(id.xy) + 0.5) * g_ViewSizeAndInvSize.zw) * float2(2.0, -2.0) + float2(-1.0, 1.0);
    const float depth = g_Depth.Load(pixel);
    const float4 prev = screen.x * g_ClipToPrevClip[0] + screen.y * g_ClipToPrevClip[1] +
                        depth * g_ClipToPrevClip[2] + g_ClipToPrevClip[3];
    velocity = screen - prev.xy / prev.w;
  }
  if (g_Mode == 2) velocity = 0.0;
  // Previous position minus this one in pixels, x right and y down.
  g_Motion[id.xy] = float2(-velocity.x, velocity.y) * 0.5 * g_ViewSizeAndInvSize.xy;
}
