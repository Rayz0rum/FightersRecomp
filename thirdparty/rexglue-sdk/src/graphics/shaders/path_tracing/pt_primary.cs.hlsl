// Primary visibility: traces a camera ray through every pixel of the scene to
// get the surface there (geometric normal and view depth) and its color, and
// writes the guides the denoisers need (depth, normals and roughness, motion,
// diffuse and specular albedo).
//
// The color is the rasterized scene's (exact, with blended effects), except
// where the game darkened it with its own shadows - they're not in the traced
// geometry, so the ray finds the ground under them, and its material color is
// used instead.
//
// Motion: the surface's triangle in the previous frame (the same draw, found
// by matching the draws of the two frames), at the same barycentrics -
// animated geometry included. Without a match, the surface is assumed static,
// moving only with the camera.
//
// Pixels without geometry (or only far scenery) show the background, whose
// average color lights the scene as the sky.

#include "pt_material.hlsli"

#define NRD_HEADER_ONLY
#include "NRD.hlsli"

RaytracingAccelerationStructure pt_scene : register(t0);
ByteAddressBuffer pt_vertices : register(t1);
RWByteAddressBuffer pt_stats : register(u1);
// View space triangles of the previous frame (same layout as pt_vertices).
ByteAddressBuffer pt_previous_vertices : register(t9);

// Output pixels.
Texture2D<float4> pt_color : register(t0, space3);
// Local pixels.
// Normal xyz, view depth w (negative for the background).
RWTexture2D<float4> pt_gbuffer_out : register(u0, space3);
// Surface color (gamma-encoded, as in the frame).
RWTexture2D<float4> pt_albedo_out : register(u1, space3);
// Where the surface was in the previous frame: output pixel xy, view depth,
// whether known.
RWTexture2D<float4> pt_motion_out : register(u2, space3);
// Denoiser guides.
RWTexture2D<float> pt_view_depth_out : register(u3, space3);
RWTexture2D<float4> pt_nrd_normal_roughness_out : register(u4, space3);
RWTexture2D<float4> pt_world_motion_out : register(u5, space3);
RWTexture2D<float4> pt_screen_motion_out : register(u6, space3);
RWTexture2D<float4> pt_normal_roughness_out : register(u7, space3);
RWTexture2D<float4> pt_octahedral_normal_out : register(u8, space3);
RWTexture2D<float4> pt_diffuse_albedo_out : register(u9, space3);
RWTexture2D<float4> pt_specular_albedo_out : register(u10, space3);

// Previous frame view space position of the surface point, and whether it's
// known exactly (from the same triangle) - otherwise it's where it would be
// if it was static.
float4 PTPreviousPosition(uint primitive, float2 barycentrics, float3 position) {
  if (pt_history_valid && PTMaterialsValid()) {
    uint draw_index = pt_attributes.Load(primitive * 48 + 28);
    if (draw_index != 0xFFFFFFFFu) {
      uint previous_first = PTDrawMaterial(draw_index).w;
      if (previous_first != 0xFFFFFFFFu) {
        uint address = (previous_first + (primitive - PTDraw(draw_index).x)) * 36;
        if (isnan(asfloat(pt_previous_vertices.Load(address)))) {
          address += kPTVertexRegionSize;
        }
        float3 p0 = asfloat(pt_previous_vertices.Load3(address));
        float3 p1 = asfloat(pt_previous_vertices.Load3(address + 12));
        float3 p2 = asfloat(pt_previous_vertices.Load3(address + 24));
        float3 previous = p0 * (1.0 - barycentrics.x - barycentrics.y) + p1 * barycentrics.x +
                          p2 * barycentrics.y;
        if (all(abs(previous) < 1.0e6)) {
          return float4(previous, 1.0);
        }
      }
    }
  }
  if (pt_flags & kPTFlagCameraTracked) {
    return float4(PTViewToPreviousView(position), 0.5);
  }
  return float4(position, 0.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  int2 local = int2(id.xy);
  bool inside = PTInRect(local);
  uint2 pixel = pt_rect_min + id.xy;
  float4 result = float4(0.0, 0.0, 0.0, -1.0);
  float3 color = float3(0.0, 0.0, 0.0);
  if (inside) {
    color = pt_color[pixel].rgb;
    float3 direction = PTPixelRay(float2(pixel) + 0.5);
    float t;
    uint primitive;
    float2 barycentrics;
    float3 albedo = color;
    float4 motion = float4(0.0, 0.0, 0.0, 0.0);
    float4 world_motion = float4(0.0, 0.0, 0.0, 0.0);
    float4 screen_motion = float4(0.0, 0.0, 0.0, 0.0);
    float3 world_normal = float3(0.0, 1.0, 0.0);
    float view_depth = kPTBackgroundDepth;
    float3 specular_albedo = float3(0.04, 0.04, 0.04);
    if (PTTraceClosest(pt_scene, float3(0.0, 0.0, 0.0), direction, 1.0e-3, pt_max_distance, t,
                       primitive, barycentrics)) {
      float3 normal = PTTriangleNormal(pt_vertices, primitive, -direction);
      // The direction has z = 1, so the distance is the view depth.
      result = float4(normal, t);
      view_depth = t;
      world_normal = PTViewToWorld(normal);
      float3 position = direction * t;

      // Motion.
      float4 previous = PTPreviousPosition(primitive, barycentrics, position);
      if (previous.w > 0.0 && previous.z > 1.0e-3) {
        float2 previous_pixel = PTProjectToPixel(previous.xyz);
        motion = float4(previous_pixel, previous.z, 1.0);
        float2 rect_size = float2(PTRectSize());
        screen_motion = float4((previous_pixel - (float2(pixel) + 0.5)) / rect_size,
                               previous.z - t, 0.0);
        // In world space, the motion of the point itself (none for static
        // geometry).
        world_motion.xyz = PTViewToWorld(PTPreviousViewToView(previous.xyz) - position);
      }

      if (PTMaterialsValid()) {
        PTSurface surface = PTMaterialSurface(primitive, barycentrics);
        uint draw_index = pt_attributes.Load(primitive * 48 + 28);
        if (surface.valid && draw_index != 0xFFFFFFFFu &&
            (PTDraw(draw_index).w & kPTDrawOpaque) && !(pt_flags & kPTFlagAlbedoRendering)) {
          // The frame has the game's lit colors - undone with the triangle's
          // lighting factor.
          float factor = dot(PTLightFactors(primitive),
                             float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x,
                                    barycentrics.y));
          albedo = PTNormalizeLighting(color, factor);
        }
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
          } else if (pt_flags & kPTFlagReplaceGameShadows) {
            // Darkened by a shadow of the game's (including its soft edges):
            // the same color as the material, scaled down - not another
            // surface drawn over it (decals and layers not in the traced
            // geometry keep their colors).
            float3 material = surface.albedo;
            float material_squared = dot(material, material);
            float scale = dot(albedo, material) / max(material_squared, 1.0e-4);
            float3 residual = albedo - material * scale;
            if (scale < 0.6 &&
                dot(residual, residual) < 0.01 * material_squared + 3.0e-3) {
              albedo = material;
            }
          }
        }
      }
      float alpha = pt_roughness * pt_roughness;
      specular_albedo =
          PTSpecularAlbedo(float3(0.04, 0.04, 0.04), alpha, dot(normal, -normalize(direction)));
    } else {
      // Background: the sky's color for the denoisers.
      albedo = color;
      specular_albedo = float3(0.5, 0.5, 0.5);
    }
    pt_gbuffer_out[local] = result;
    pt_albedo_out[local] = float4(albedo, 1.0);
    pt_motion_out[local] = motion;
    pt_view_depth_out[local] = view_depth;
    pt_nrd_normal_roughness_out[local] =
        NRD_FrontEnd_PackNormalAndRoughness(world_normal, pt_roughness, 0.0);
    pt_world_motion_out[local] = world_motion;
    pt_screen_motion_out[local] = screen_motion;
    pt_normal_roughness_out[local] = float4(world_normal, pt_roughness);
    pt_octahedral_normal_out[local] = float4(PTNormalToOctahedron(world_normal), pt_roughness, 0.0);
    pt_diffuse_albedo_out[local] = float4(PTLinear(albedo), 1.0);
    pt_specular_albedo_out[local] = float4(specular_albedo, 1.0);
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
