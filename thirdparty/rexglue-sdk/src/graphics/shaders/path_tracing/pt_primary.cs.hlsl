// Primary visibility: traces a camera ray through every pixel of the scene to
// get the surface there (geometric normal and view depth) and its color.
//
// The color is the rasterized scene's (exact, with blended effects), except
// where the game darkened it with its own shadows - they're not in the traced
// geometry, so the ray finds the ground under them, and its material color is
// used instead.
//
// Pixels without geometry (or only far scenery) show the background, whose
// average color lights the scene as the sky.

#include "pt_material.hlsli"

RaytracingAccelerationStructure pt_scene : register(t0);
ByteAddressBuffer pt_vertices : register(t1);
RWByteAddressBuffer pt_stats : register(u1);
Texture2D<float4> pt_color : register(t2);
RWTexture2D<float4> pt_gbuffer_out : register(u2);
RWTexture2D<float4> pt_albedo_out : register(u3);
// Where the surface was in the previous frame: output pixel xy, view depth,
// whether known.
RWTexture2D<float4> pt_motion_out : register(u5);
// View space triangles of the previous frame (same layout as pt_vertices).
ByteAddressBuffer pt_previous_vertices : register(t9);

float4 PTPreviousPosition(uint primitive, float2 barycentrics) {
  if (!pt_history_valid || !PTMaterialsValid()) {
    return float4(0.0, 0.0, 0.0, 0.0);
  }
  uint draw_index = pt_attributes.Load(primitive * 48 + 28);
  if (draw_index == 0xFFFFFFFFu) {
    return float4(0.0, 0.0, 0.0, 0.0);
  }
  uint previous_first = PTDrawMaterial(draw_index).w;
  if (previous_first == 0xFFFFFFFFu) {
    return float4(0.0, 0.0, 0.0, 0.0);
  }
  uint address = (previous_first + (primitive - PTDraw(draw_index).x)) * 36;
  if (isnan(asfloat(pt_previous_vertices.Load(address)))) {
    address += kPTVertexRegionSize;
  }
  float3 p0 = asfloat(pt_previous_vertices.Load3(address));
  float3 p1 = asfloat(pt_previous_vertices.Load3(address + 12));
  float3 p2 = asfloat(pt_previous_vertices.Load3(address + 24));
  float3 position = p0 * (1.0 - barycentrics.x - barycentrics.y) + p1 * barycentrics.x +
                    p2 * barycentrics.y;
  if (!all(abs(position) < 1.0e6) || position.z <= 1.0e-3) {
    return float4(0.0, 0.0, 0.0, 0.0);
  }
  return float4(PTProjectToPixel(position), position.z, 1.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint2 pixel = pt_rect_min + id.xy;
  bool inside = all(pixel < pt_rect_max);
  float4 result = float4(0.0, 0.0, 0.0, -1.0);
  float3 color = float3(0.0, 0.0, 0.0);
  float4 motion = float4(0.0, 0.0, 0.0, 0.0);
  if (inside) {
    color = pt_color[pixel].rgb;
    float3 direction = PTPixelRay(float2(pixel) + 0.5);
    float t;
    uint primitive;
    float2 barycentrics;
    if (PTTraceClosest(pt_scene, float3(0.0, 0.0, 0.0), direction, 1.0e-3, pt_max_distance, t,
                       primitive, barycentrics)) {
      float3 normal = PTTriangleNormal(pt_vertices, primitive, -direction);
      // The direction has z = 1, so the distance is the view depth.
      result = float4(normal, t);
      motion = PTPreviousPosition(primitive, barycentrics);
      float3 albedo = color;
      if (PTMaterialsValid()) {
        PTSurface surface = PTMaterialSurface(primitive, barycentrics);
        if (surface.valid) {
          if (pt_debug_view == 6) {
            albedo = surface.albedo;
          } else if (pt_debug_view == 7) {
            float3 factors = PTLightFactors(primitive);
            float factor = dot(factors, float3(1.0 - barycentrics.x - barycentrics.y,
                                              barycentrics.x, barycentrics.y));
            float spread = max(max(factors.x, factors.y), factors.z) -
                           min(min(factors.x, factors.y), factors.z);
            albedo = float3(factor, factor, saturate(spread * 4.0));
          } else if ((pt_flags & kPTFlagReplaceGameShadows) &&
                     dot(color, kPTLuminance) < dot(surface.albedo, kPTLuminance) * 0.6) {
            // Darkened by a shadow of the game's (including its soft edges).
            albedo = surface.albedo;
          }
        }
      }
      pt_albedo_out[pixel] = float4(albedo, 1.0);
    } else {
      pt_albedo_out[pixel] = float4(color, 1.0);
    }
    pt_gbuffer_out[pixel] = result;
    pt_motion_out[pixel] = motion;
  }

  // Background color statistics.
  bool background = inside && result.w < 0.0;
  uint3 background_color =
      background ? uint3(saturate(color) * 255.0 + 0.5) : uint3(0, 0, 0);
  uint3 color_sum = WaveActiveSum(background_color);
  uint count = WaveActiveCountBits(background);
  if (WaveIsFirstLane() && count != 0) {
    uint base = pt_stats_slot * kPTStatsSlotSize;
    pt_stats.InterlockedAdd(base + 16, color_sum.r);
    pt_stats.InterlockedAdd(base + 20, color_sum.g);
    pt_stats.InterlockedAdd(base + 24, color_sum.b);
    pt_stats.InterlockedAdd(base + 28, count);
  }
}
