// Shared definitions for the experimental path-traced lighting passes.
//
// Built with the Windows SDK DirectX Shader Compiler (shader model 6.5 for
// inline ray queries), see build_pt_shaders.cmd.
//
// View space here is the guest's: x right, y up, z (= clip w) forward, with
// the camera at the origin.

#ifndef PT_COMMON_HLSLI_
#define PT_COMMON_HLSLI_

cbuffer PTConstants : register(b0) {
  // 1 / projection scale (guest clip xy = view xy * proj).
  float2 pt_inv_proj;
  // Host clip xy = guest clip xy * ndc_scale + ndc_offset * w.
  float2 pt_ndc_scale;
  float2 pt_ndc_offset;
  // Host viewport in output pixels.
  float2 pt_viewport_offset;
  float2 pt_viewport_extent;
  // Output pixels the scene covers, max exclusive.
  uint2 pt_rect_min;
  uint2 pt_rect_max;
  uint2 pt_output_size;
  // Vertices in the acceleration structure (triangle list).
  uint pt_vertex_count;
  // Which half of the statistics buffer this frame accumulates into.
  uint pt_stats_slot;
  float pt_gi_distance;
  // How much of the game's own lighting is replaced (0 to 1).
  float pt_strength;
  float3 pt_sun_color;
  // Multiplier of the light from the background (sky).
  float pt_sky_scale;
  // Sun elevation above the estimated ground plane and azimuth relative to
  // the camera, in radians.
  float2 pt_sun_angles;
  // Sun cone half-angle tangent, for soft shadows.
  float pt_sun_softness;
  uint pt_ray_count;
  uint pt_debug_view;
  // Specular reflection strength (0 disables the reflection rays).
  float pt_specular;
  // Denoiser pass: distance between taps and taps each side.
  uint pt_filter_step;
  float pt_bounce_scale;
  float pt_ambient;
  // Average brightness auto exposure aims for, relative to the original.
  float pt_exposure_target;
  float pt_shadow_distance;
  uint pt_filter_radius;
  uint pt_flags;
  float pt_sky_saturation;
  // Surfaces further away are scenery (sky, clouds, distant landscape) and
  // are left as they are, like the background.
  float pt_max_distance;
  // GGX roughness of the surfaces.
  float pt_roughness;
};

// pt_flags.
static const uint kPTFlagReplaceGameShadows = 1u << 0;

// Statistics buffer, per frame slot:
// +0 int3 - sum of up-facing triangle normals (ground plane estimate).
// +16 uint3 - sum of background (no geometry) pixel colors, x255.
// +28 uint - background pixel count.
// +32 uint - sum of lighting luminance, x256.
// +36 uint - lit pixel count.
// Not cleared between frames, from +128, per frame slot:
// +0 float3 - sky light color smoothed over time.
static const uint kPTStatsSlotSize = 64;
static const uint kPTStatsSkyOffset = 128;
static const float kPTStatsScale = 4096.0;

static const float3 kPTLuminance = float3(0.2126, 0.7152, 0.0722);

// Ground normal from the up-facing triangle normal sums.
float3 PTGroundUp(uint3 sum_bits) {
  float3 up = float3(asint(sum_bits)) * (1.0 / kPTStatsScale);
  float up_length = length(up);
  return up_length > 1.0e-3 ? up / up_length : float3(0.0, 1.0, 0.0);
}

// View space direction (with z = 1, so the ray distance is the view depth)
// through a point in output pixels.
float3 PTPixelRay(float2 pixel) {
  float2 host_ndc =
      float2((pixel.x - pt_viewport_offset.x) / pt_viewport_extent.x * 2.0 - 1.0,
             1.0 - (pixel.y - pt_viewport_offset.y) / pt_viewport_extent.y * 2.0);
  float2 guest_ndc = (host_ndc - pt_ndc_offset) / pt_ndc_scale;
  return float3(guest_ndc * pt_inv_proj, 1.0);
}

// Output pixel position of a view space point in front of the camera.
float2 PTProjectToPixel(float3 p) {
  float2 guest_ndc = p.xy / (pt_inv_proj * p.z);
  float2 host_ndc = guest_ndc * pt_ndc_scale + pt_ndc_offset;
  return float2(pt_viewport_offset.x + (host_ndc.x * 0.5 + 0.5) * pt_viewport_extent.x,
                pt_viewport_offset.y + (0.5 - host_ndc.y * 0.5) * pt_viewport_extent.y);
}

bool PTInRect(int2 pixel) {
  return all(pixel >= int2(pt_rect_min)) && all(pixel < int2(pt_rect_max));
}

float PTRadicalInverse(uint bits) {
  return float(reversebits(bits)) * 2.3283064365386963e-10;
}

void PTBasis(float3 n, out float3 t, out float3 b) {
  // Duff et al., "Building an Orthonormal Basis, Revisited".
  float s = n.z >= 0.0 ? 1.0 : -1.0;
  float a = -1.0 / (s + n.z);
  float c = n.x * n.y * a;
  t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
  b = float3(c, s + n.y * n.y * a, -n.y);
}

// Edge-stopping weight between two G-buffer samples (normal xyz, depth w).
float PTSurfaceWeight(float4 center, float4 tap) {
  if (tap.w <= 0.0) {
    return 0.0;
  }
  float n_dot_n = saturate(dot(tap.xyz, center.xyz));
  float n_weight = n_dot_n * n_dot_n;
  n_weight *= n_weight;
  n_weight *= n_weight;
  return exp(-abs(tap.w - center.w) / (0.02 * center.w + 1.0e-3)) * n_weight;
}

#endif  // PT_COMMON_HLSLI_
