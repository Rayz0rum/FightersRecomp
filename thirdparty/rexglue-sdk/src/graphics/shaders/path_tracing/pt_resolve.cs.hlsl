// Combines the surface colors with the denoised traced lighting into exposed
// linear HDR color: surface color x irradiance + specular. Alpha is whether
// the pixel is lit by the path tracer (scene geometry), 0 for the background.

#include "pt_common.hlsli"

RWByteAddressBuffer pt_stats : register(u1);
Texture2D<float4> pt_gbuffer : register(t3);
Texture2D<float4> pt_lighting : register(t4);
Texture2D<float4> pt_albedo : register(t6);
// Specular radiance (denoised).
Texture2D<float4> pt_specular_radiance : register(t0, space2);
RWTexture2D<float4> pt_hdr_out : register(u2);

float3 PTLinear(float3 color) { return pow(max(color, 0.0), 2.2); }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint2 pixel = pt_rect_min + id.xy;
  if (any(pixel >= pt_rect_max)) {
    return;
  }
  float4 hdr = float4(0.0, 0.0, 0.0, 0.0);
  if (pt_gbuffer[pixel].w > 0.0) {
    uint2 average = pt_stats.Load2(pt_stats_slot * kPTStatsSlotSize + 32);
    float average_irradiance =
        average.y != 0 ? float(average.x) / (32.0 * float(average.y)) : 1.0;
    float exposure = clamp(pt_exposure_target / max(average_irradiance, 1.0e-3), 0.25, 4.0);
    hdr.rgb = (PTLinear(pt_albedo[pixel].rgb) * pt_lighting[pixel].rgb +
               pt_specular_radiance[pixel].rgb) *
              exposure;
    hdr.a = 1.0;
  }
  pt_hdr_out[pixel] = hdr;
}
