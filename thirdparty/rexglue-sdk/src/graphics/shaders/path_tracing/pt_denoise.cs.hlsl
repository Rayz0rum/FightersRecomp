// One pass of the built-in edge-aware (depth and normal) filter of the traced
// lighting. Run a few times with increasing tap spacing (a-trous).

#include "pt_common.hlsli"

// All local pixels.
Texture2D<float4> pt_gbuffer : register(t0, space3);
Texture2D<float4> pt_lighting : register(t1, space3);
RWTexture2D<float4> pt_lighting_out : register(u0, space3);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  int2 pixel = int2(id.xy);
  if (!PTInRect(pixel)) {
    return;
  }
  float4 surface = pt_gbuffer[pixel];
  if (surface.w <= 0.0) {
    pt_lighting_out[pixel] = pt_lighting[pixel];
    return;
  }
  int radius = int(pt_filter_radius);
  int step = int(pt_filter_step);
  float spatial_falloff = 2.0 / float(max(radius * radius, 1));
  float4 sum = float4(0.0, 0.0, 0.0, 0.0);
  float weight_sum = 0.0;
  for (int y = -radius; y <= radius; ++y) {
    for (int x = -radius; x <= radius; ++x) {
      int2 tap = pixel + int2(x, y) * step;
      if (!PTInRect(tap)) {
        continue;
      }
      float weight = PTSurfaceWeight(surface, pt_gbuffer[tap]) *
                     exp(-float(x * x + y * y) * spatial_falloff);
      sum += pt_lighting[tap] * weight;
      weight_sum += weight;
    }
  }
  pt_lighting_out[pixel] = weight_sum > 1.0e-5 ? sum / weight_sum : pt_lighting[pixel];
}
