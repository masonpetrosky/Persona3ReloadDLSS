// The pixel shader of the HUD fix's clamp pipelines. They redraw a HUD draw's geometry with an alpha-only blend:
// VALUE 0 with a Maximum blend lifts alpha below 0 to 0, VALUE 1 with a Minimum blend lowers alpha above 1 to 1.
// No inputs, so it links with any vertex shader.
// SPDX-License-Identifier: MIT

float4 main() : SV_Target0 {
  return float4(0.0, 0.0, 0.0, VALUE);
}
