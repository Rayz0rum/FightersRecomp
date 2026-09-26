// Path traces the lighting of the primary surface of every scene pixel, in
// linear HDR (not multiplied by the surface color - the resolve does that):
// - sun light, with shadow rays over the sun's disk,
// - global illumination: cosine-distributed rays that either escape to the
//   sky (colored like the frame's background) or hit a surface, which reflects
//   the sun (with its own shadow ray) and the sky, with its material's color.
// - specular: GGX highlights of the sun and a ray traced glossy reflection of
//   the scene (dielectric, Fresnel-weighted).
// Alpha-tested geometry (foliage, fences) lets light through its holes.
//
// Outputs, depending on the denoiser:
// - built-in and DLSS Ray Reconstruction: total irradiance (sun and indirect)
//   and total specular.
// - NRD and FSR Ray Regeneration: separate signals - sun visibility (the
//   shadow ray's occluder distance), indirect diffuse irradiance and indirect
//   specular (divided by the specular albedo) with their hit distances. The
//   sun's direct light and highlight are added back analytically with the
//   denoised visibility.

#include "pt_material.hlsli"

#define NRD_HEADER_ONLY
#include "NRD.hlsli"

RaytracingAccelerationStructure pt_scene : register(t0);
ByteAddressBuffer pt_vertices : register(t1);
RWByteAddressBuffer pt_stats : register(u1);
// Output pixels.
Texture2D<float4> pt_color : register(t0, space3);
// Local pixels.
Texture2D<float4> pt_gbuffer : register(t1, space3);
Texture2D<float4> pt_specular_albedo : register(t2, space3);
// The sky around the scene by world direction (octahedral, see pt_sky).
Texture2D<float4> pt_sky_map : register(t3, space3);
// World normal, roughness (w).
Texture2D<float4> pt_normal_roughness : register(t4, space3);
// Total irradiance / total specular, or indirect diffuse / indirect specular /
// sun visibility (NRD penumbra or FSR occluder distance, the occluder
// distance, visibility).
RWTexture2D<float4> pt_lighting_out : register(u0, space3);
RWTexture2D<float4> pt_specular_out : register(u1, space3);
RWTexture2D<float4> pt_shadow_out : register(u2, space3);

// Radiance of the sky in a view space direction: what the background showed
// in that direction, or the average background where it hasn't been seen.
float3 PTSky(float3 sky, float3 up, float3 direction) {
  float3 average = sky * (0.6 + 0.4 * dot(direction, up));
  float4 learned = pt_sky_map.SampleLevel(
      pt_sampler_linear_clamp, PTNormalToOctahedron(PTViewToWorld(direction)), 0.0);
  if (learned.a <= 0.0) {
    return average;
  }
  float3 color = lerp(dot(learned.rgb, kPTLuminance).xxx, learned.rgb, pt_sky_saturation) *
                     pt_sky_scale +
                 pt_ambient;
  return lerp(average, color, learned.a);
}

// Light a hit surface reflects towards the ray: the sun if it reaches it, and
// the sky (roughly half of it visible), times its material color.
float3 PTHitRadiance(float3 origin, float3 direction, float t, uint primitive, float2 barycentrics,
                     float3 sky, float3 up, float3 sun, bool materials) {
  float3 hit = origin + direction * t;
  float3 hit_normal = PTTriangleNormal(pt_vertices, primitive, -direction);
  float3 albedo = float3(0.35, 0.35, 0.35);
  PTSurface hit_material;
  hit_material.valid = false;
  if (materials) {
    hit_material = PTMaterialSurface(primitive, barycentrics);
  }
  if (hit_material.valid) {
    albedo = hit_material.albedo;
  } else if (hit.z > 1.0e-3) {
    int2 hit_local = int2(PTProjectToPixel(hit)) - int2(pt_rect_min);
    if (PTInRect(hit_local)) {
      albedo = lerp(albedo, pt_color[pt_rect_min + uint2(hit_local)].rgb, 0.5);
    }
  }
  float3 hit_irradiance = PTSky(sky, up, hit_normal) * 0.5;
  float hit_n_dot_l = dot(hit_normal, sun);
  if (hit_n_dot_l > 0.0) {
    float hit_bias = 2.0e-3 * max(hit.z, 0.0) + 1.0e-3;
    if (!PTTraceAny(pt_scene, hit + hit_normal * hit_bias, sun, pt_shadow_distance)) {
      hit_irradiance += pt_sun_color * hit_n_dot_l;
    }
  }
  return PTLinear(albedo) * hit_irradiance;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  int2 local = int2(id.xy);
  bool inside = PTInRect(local);
  uint2 pixel = pt_rect_min + id.xy;
  float4 surface = inside ? pt_gbuffer[local] : float4(0.0, 0.0, 0.0, -1.0);
  bool lit = surface.w > 0.0;
  bool split = PTSplitSignals();

  // Sky light from the average background, mostly neutral (the background
  // may be anything, like a big blue wall) and smoothed over time so turning
  // the camera doesn't change it abruptly. Every thread computes the same
  // value, one stores it for the next frame.
  uint stats_base = pt_stats_slot * kPTStatsSlotSize;
  uint4 background = pt_stats.Load4(stats_base + 16);
  float3 sky = background.w != 0 ? float3(background.rgb) / (255.0 * float(background.w))
                                 : float3(0.5, 0.5, 0.5);
  sky = lerp(dot(sky, kPTLuminance).xxx, sky, pt_sky_saturation);
  float3 previous_sky =
      asfloat(pt_stats.Load3(kPTStatsSkyOffset + (pt_stats_slot ^ 1) * 16));
  if (any(previous_sky > 0.0)) {
    sky = lerp(previous_sky, sky, 0.05);
  }
  if (all(id.xy == 0)) {
    pt_stats.Store3(kPTStatsSkyOffset + pt_stats_slot * 16, asuint(sky));
  }
  sky = PTLinear(sky) * pt_sky_scale + pt_ambient;

  float3 total_irradiance = float3(1.0, 1.0, 1.0);
  float4 lighting = float4(0.0, 0.0, 0.0, 0.0);
  float4 specular = float4(0.0, 0.0, 0.0, 0.0);
  float4 shadow = float4(0.0, 0.0, 0.0, 0.0);
  if (lit) {
    float3 up = PTGroundUp(pt_stats.Load3(stats_base));
    float3 sun = PTSunDirection(pt_stats.Load4(kPTStatsSunOffset + pt_stats_slot * 16), up);

    float3 normal = surface.xyz;
    float3 position = PTPixelRay(float2(pixel) + 0.5) * surface.w;
    float bias = 2.0e-3 * surface.w + 1.0e-3;
    float3 origin = position + normal * bias;
    float3 view = -normalize(position);
    float n_dot_v = max(dot(normal, view), 1.0e-3);
    float roughness = pt_normal_roughness[local].w;
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    bool materials = PTMaterialsValid();
    uint ray_count = max(pt_ray_count, 1u);

    // Direct sun light: shadow rays over the sun's disk.
    float sun_visibility = 0.0;
    float occluder_distance = kPTFP16Max;
    float n_dot_l = dot(normal, sun);
    if (n_dot_l > 0.0) {
      float3 sun_tangent, sun_bitangent;
      PTBasis(sun, sun_tangent, sun_bitangent);
      uint seed = PTRandomSeed(pixel, 0);
      // One ray for the denoisers working on the visibility (it's the
      // distribution they expect), more for the others.
      uint shadow_rays = split ? 1u : ray_count;
      uint unshadowed = 0;
      for (uint i = 0; i < shadow_rays; ++i) {
        float2 u = PTRandom2(seed);
        float phi = 6.28318530718 * u.y;
        float r = sqrt(u.x) * pt_sun_softness;
        float3 direction =
            normalize(sun + sun_tangent * (r * cos(phi)) + sun_bitangent * (r * sin(phi)));
        if (split) {
          float t;
          uint primitive;
          float2 barycentrics;
          if (PTTraceClosest(pt_scene, origin, direction, 0.0, pt_shadow_distance, t, primitive,
                             barycentrics)) {
            occluder_distance = min(occluder_distance, t);
          } else {
            ++unshadowed;
          }
        } else if (!PTTraceAny(pt_scene, origin, direction, pt_shadow_distance)) {
          ++unshadowed;
        }
      }
      sun_visibility = float(unshadowed) / float(shadow_rays);
    } else {
      // Facing away from the sun: fully shadowed at no distance.
      occluder_distance = 0.0;
    }
    float3 direct = pt_sun_color * (max(n_dot_l, 0.0) * sun_visibility);

    // Global illumination.
    float3 tangent, bitangent;
    PTBasis(normal, tangent, bitangent);
    float3 indirect = float3(0.0, 0.0, 0.0);
    float hit_distance_sum = 0.0;
    uint seed = PTRandomSeed(pixel, 1);
    for (uint i = 0; i < ray_count; ++i) {
      float2 u = PTRandom2(seed);
      float phi = 6.28318530718 * u.y;
      float r = sqrt(u.x);
      float3 direction = tangent * (r * cos(phi)) + bitangent * (r * sin(phi)) +
                         normal * sqrt(max(1.0 - u.x, 0.0));
      float t;
      uint primitive;
      float2 barycentrics;
      if (!PTTraceClosest(pt_scene, origin, direction, 0.0, pt_gi_distance, t, primitive,
                          barycentrics)) {
        indirect += PTSky(sky, up, direction);
        hit_distance_sum += pt_gi_distance;
        continue;
      }
      indirect += PTHitRadiance(origin, direction, t, primitive, barycentrics, sky, up, sun,
                                materials) *
                  pt_bounce_scale;
      hit_distance_sum += t;
    }
    indirect /= float(ray_count);
    float hit_distance = hit_distance_sum / float(ray_count);

    // Glossy reflection of the scene: one GGX-distributed ray.
    float3 reflection = float3(0.0, 0.0, 0.0);
    float reflection_distance = 0.0;
    if (pt_specular > 0.0) {
      uint reflection_seed = PTRandomSeed(pixel, 2);
      float2 u = PTRandom2(reflection_seed);
      // Microfacet normal from the GGX distribution.
      float cos_theta = sqrt((1.0 - u.x) / (1.0 + (alpha2 - 1.0) * u.x));
      float sin_theta = sqrt(max(1.0 - cos_theta * cos_theta, 0.0));
      float phi = 6.28318530718 * u.y;
      float3 half_vector = normalize(tangent * (sin_theta * cos(phi)) +
                                     bitangent * (sin_theta * sin(phi)) + normal * cos_theta);
      float3 direction = reflect(-view, half_vector);
      float reflection_n_dot_l = dot(normal, direction);
      if (reflection_n_dot_l > 0.0) {
        float v_dot_h = saturate(dot(view, half_vector));
        float n_dot_h = max(dot(normal, half_vector), 1.0e-3);
        // Importance sampled: F * G * (v.h) / (n.h * n.v).
        float weight = PTFresnel(v_dot_h) * PTSmithG1(reflection_n_dot_l, alpha2) *
                       PTSmithG1(n_dot_v, alpha2) * v_dot_h / (n_dot_h * n_dot_v);
        float t;
        uint primitive;
        float2 barycentrics;
        float3 radiance;
        if (!PTTraceClosest(pt_scene, origin, direction, 0.0, pt_gi_distance, t, primitive,
                            barycentrics)) {
          radiance = PTSky(sky, up, direction);
          reflection_distance = pt_gi_distance;
        } else {
          radiance = PTHitRadiance(origin, direction, t, primitive, barycentrics, sky, up, sun,
                                   materials);
          reflection_distance = t;
        }
        reflection = radiance * weight;
      }
    }

    total_irradiance = direct + indirect;
    if (split) {
      float3 specular_albedo = max(pt_specular_albedo[local].rgb, 1.0e-3);
      float3 demodulated_reflection = reflection / specular_albedo;
      // NRD (SIGMA): penumbra size, FSR: occluder distance (FP16 max if none).
      float fsr_distance = min(occluder_distance, kPTFP16Max);
      shadow = float4(pt_denoiser == kPTDenoiserNRD
                          ? SIGMA_FrontEnd_PackPenumbra(occluder_distance, pt_sun_softness)
                          : fsr_distance,
                      fsr_distance, sun_visibility, 1.0);
      if (pt_denoiser == kPTDenoiserNRD) {
        float3 hit_parameters = pt_nrd_hit_distance_parameters.xyz;
        lighting = REBLUR_FrontEnd_PackRadianceAndNormHitDist(
            indirect, REBLUR_FrontEnd_GetNormHitDist(hit_distance, surface.w, hit_parameters, 1.0),
            true);
        specular = REBLUR_FrontEnd_PackRadianceAndNormHitDist(
            demodulated_reflection,
            REBLUR_FrontEnd_GetNormHitDist(reflection_distance, surface.w, hit_parameters,
                                           roughness),
            true);
      } else {
        lighting = float4(indirect, hit_distance);
        specular = float4(demodulated_reflection, reflection_distance);
      }
    } else {
      float highlight =
          PTSpecularHighlight(normal, view, sun, roughness) * sun_visibility * pt_specular;
      lighting = float4(total_irradiance, 1.0);
      specular = float4(min(pt_sun_color * highlight + reflection * pt_specular, 64.0), 1.0);
    }
  } else if (split) {
    // Background: nothing to denoise.
    shadow = float4(kPTFP16Max, kPTFP16Max, 1.0, 0.0);
    lighting = pt_denoiser == kPTDenoiserNRD ? float4(0.0, 0.0, 0.0, 1.0)
                                             : float4(0.0, 0.0, 0.0, -1.0);
    specular = lighting;
  }
  if (inside) {
    pt_lighting_out[local] = lighting;
    pt_specular_out[local] = specular;
    if (split) {
      pt_shadow_out[local] = shadow;
    }
  }

  // Average brightness for the auto exposure.
  uint luminance = lit ? uint(min(dot(total_irradiance, kPTLuminance), 64.0) * 32.0) : 0;
  uint luminance_sum = WaveActiveSum(luminance);
  uint lit_count = WaveActiveCountBits(lit);
  if (WaveIsFirstLane() && lit_count != 0) {
    uint base = pt_stats_slot * kPTStatsSlotSize;
    pt_stats.InterlockedAdd(base + 32, luminance_sum);
    pt_stats.InterlockedAdd(base + 36, lit_count);
  }
}
