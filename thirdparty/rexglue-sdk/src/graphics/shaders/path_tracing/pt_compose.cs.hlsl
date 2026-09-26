// The noisy lit color for DLSS Ray Reconstruction (which denoises it with the
// albedo, normal, depth and motion guides): surface color x irradiance +
// specular, linear HDR, not exposed. The background keeps the frame's color.

#include "pt_common.hlsli"

// Output pixels.
Texture2D<float4> pt_color : register(t0, space3);
// Local pixels.
Texture2D<float4> pt_gbuffer : register(t1, space3);
Texture2D<float4> pt_albedo : register(t2, space3);
Texture2D<float4> pt_lighting : register(t3, space3);
Texture2D<float4> pt_specular_radiance : register(t4, space3);
RWTexture2D<float4> pt_color_out : register(u0, space3);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  int2 local = int2(id.xy);
  if (!PTInRect(local)) {
    return;
  }
  float3 color;
  if (pt_gbuffer[local].w > 0.0) {
    color = PTLinear(pt_albedo[local].rgb) * pt_lighting[local].rgb +
            pt_specular_radiance[local].rgb;
  } else {
    color = PTLinear(pt_color[pt_rect_min + uint2(local)].rgb);
  }
  pt_color_out[local] = float4(color, 1.0);
}
