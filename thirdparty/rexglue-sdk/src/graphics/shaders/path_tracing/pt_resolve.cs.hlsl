// Combines the surface colors with the denoised traced lighting into exposed
// linear HDR color. Alpha is whether the pixel is lit by the path tracer
// (scene geometry), 0 for the background.
//
// Per denoiser:
// - built-in: surface color x irradiance + specular.
// - NRD, FSR Ray Regeneration: the sun's light and highlight with the
//   denoised visibility, plus the denoised indirect diffuse (x surface color)
//   and specular (x specular albedo).
// - DLSS Ray Reconstruction: its output is already the lit color.

#include "pt_common.hlsli"

#define NRD_HEADER_ONLY
#include "NRD.hlsli"

RWByteAddressBuffer pt_stats : register(u1);
// All local pixels.
Texture2D<float4> pt_gbuffer : register(t0, space3);
Texture2D<float4> pt_albedo : register(t1, space3);
Texture2D<float4> pt_specular_albedo : register(t2, space3);
// Irradiance, indirect diffuse, or the lit color (per denoiser).
Texture2D<float4> pt_lighting : register(t3, space3);
// Specular or indirect specular.
Texture2D<float4> pt_specular_radiance : register(t4, space3);
// Denoised sun visibility.
Texture2D<float4> pt_shadow : register(t5, space3);
// World normal, roughness (w).
Texture2D<float4> pt_normal_roughness : register(t6, space3);
RWTexture2D<float4> pt_hdr_out : register(u0, space3);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  int2 local = int2(id.xy);
  if (!PTInRect(local)) {
    return;
  }
  float4 hdr = float4(0.0, 0.0, 0.0, 0.0);
  float4 surface = pt_gbuffer[local];
  if (surface.w > 0.0) {
    uint2 average = pt_stats.Load2(pt_stats_slot * kPTStatsSlotSize + 32);
    float average_irradiance =
        average.y != 0 ? float(average.x) / (32.0 * float(average.y)) : 1.0;
    float exposure = clamp(pt_exposure_target / max(average_irradiance, 1.0e-3), 0.25, 4.0);
    float3 albedo = PTLinear(pt_albedo[local].rgb);
    float3 color;
    if (pt_denoiser == kPTDenoiserDLSSRR) {
      color = pt_lighting[local].rgb;
    } else if (PTSplitSignals()) {
      float3 up = PTGroundUp(pt_stats.Load3(pt_stats_slot * kPTStatsSlotSize));
      float3 sun = PTSunDirection(pt_stats.Load4(kPTStatsSunOffset + pt_stats_slot * 16), up);
      float3 normal = surface.xyz;
      float3 view = -normalize(PTPixelRay(float2(pt_rect_min + uint2(local)) + 0.5));
      float shadow;
      float3 diffuse, specular;
      if (pt_denoiser == kPTDenoiserNRD) {
        shadow = SIGMA_BackEnd_UnpackShadow(pt_shadow[local].x);
        diffuse = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(pt_lighting[local]).rgb;
        specular = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(pt_specular_radiance[local]).rgb;
      } else {
        shadow = pt_shadow[local].x;
        diffuse = pt_lighting[local].rgb;
        specular = pt_specular_radiance[local].rgb;
      }
      shadow = saturate(shadow);
      diffuse = max(diffuse, 0.0);
      specular = max(specular, 0.0);
      float3 irradiance = pt_sun_color * (max(dot(normal, sun), 0.0) * shadow) + diffuse;
      float highlight =
          PTSpecularHighlight(normal, view, sun, pt_normal_roughness[local].w) * shadow;
      color = albedo * irradiance +
              (specular * pt_specular_albedo[local].rgb + pt_sun_color * highlight) * pt_specular;
      if (pt_debug_view == 8) {
        color = shadow.xxx;
      } else if (pt_debug_view == 9) {
        color = diffuse;
      } else if (pt_debug_view == 10) {
        color = specular * pt_specular_albedo[local].rgb;
      }
    } else {
      color = albedo * pt_lighting[local].rgb + pt_specular_radiance[local].rgb;
    }
    hdr = float4(max(color, 0.0) * exposure, 1.0);
  }
  pt_hdr_out[local] = hdr;
}
