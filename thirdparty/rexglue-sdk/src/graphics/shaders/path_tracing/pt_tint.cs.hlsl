// Finds the tint the game applies when drawing the scene into the final frame
// (the scene times a color - fades between rounds and such): the median ratio
// of the final frame to the scene over a grid of samples (the HUD covering
// some of them doesn't matter), stored for the composite.

#include "pt_common.hlsli"

RWByteAddressBuffer pt_stats : register(u1);
// Output pixels.
Texture2D<float4> pt_color : register(t0, space3);
Texture2D<float4> pt_final_frame : register(t1, space3);

groupshared float pt_tint_ratios[3][64];

[numthreads(64, 1, 1)]
void main(uint3 thread : SV_GroupThreadID) {
  uint lane = thread.x;
  uint2 rect_size = PTRectSize();
  uint2 sample_pixel = pt_rect_min + uint2((float2(lane & 7, lane >> 3) + 0.5) / 8.0 *
                                           float2(rect_size));
  float3 scene = pt_color[sample_pixel].rgb;
  float3 frame = pt_final_frame[sample_pixel].rgb;
  [unroll] for (uint i = 0; i < 3; ++i) {
    // Dark samples don't tell - neutral.
    pt_tint_ratios[i][lane] = scene[i] > 0.05 ? frame[i] / scene[i] : -1.0;
  }
  GroupMemoryBarrierWithGroupSync();
  if (lane < 3) {
    // Median of the valid ratios (insertion sort, 64 values).
    float values[64];
    uint count = 0;
    for (uint j = 0; j < 64; ++j) {
      float value = pt_tint_ratios[lane][j];
      if (value < 0.0) {
        continue;
      }
      uint k = count++;
      while (k > 0 && values[k - 1] > value) {
        values[k] = values[k - 1];
        --k;
      }
      values[k] = value;
    }
    float tint = count >= 4 ? saturate(values[count / 2]) : 1.0;
    // Snap the usual full brightness exactly.
    if (abs(tint - 1.0) < 0.01) {
      tint = 1.0;
    }
    pt_stats.Store(kPTStatsTintOffset + lane * 4, asuint(tint));
  }
}
