// Path traces the lighting of the primary surface of every scene pixel, in
// linear HDR (the composite multiplies it with the surface color, exposes and
// tone maps it):
// - sun light, with soft shadow rays over the sun's disk,
// - global illumination: cosine-distributed rays that either escape to the
//   sky (colored like the frame's background) or hit a surface, which reflects
//   the sun (with its own shadow ray) and the sky, with its material's color.
// - specular: GGX highlights of the sun and a ray traced glossy reflection of
//   the scene (dielectric, Fresnel-weighted), output separately as it isn't
//   multiplied by the surface color.
// Alpha-tested geometry (foliage, fences) lets light through its holes.

#include "pt_material.hlsli"

RaytracingAccelerationStructure pt_scene : register(t0);
ByteAddressBuffer pt_vertices : register(t1);
RWByteAddressBuffer pt_stats : register(u1);
Texture2D<float4> pt_color : register(t2);
Texture2D<float4> pt_gbuffer : register(t3);
RWTexture2D<float4> pt_lighting_out : register(u2);
RWTexture2D<float4> pt_specular_out : register(u3);

static const float kPTPi = 3.14159265359;

float PTFresnel(float cosine) {
  // Schlick's approximation for a dielectric (F0 = 0.04).
  float f = 1.0 - saturate(cosine);
  float f2 = f * f;
  return 0.04 + 0.96 * f2 * f2 * f;
}

// Smith G1 for GGX.
float PTSmithG1(float n_dot_x, float alpha2) {
  return 2.0 * n_dot_x / (n_dot_x + sqrt(alpha2 + (1.0 - alpha2) * n_dot_x * n_dot_x));
}

// Radiance of the sky in a direction.
float3 PTSky(float3 sky, float3 up, float3 direction) {
  return sky * (0.6 + 0.4 * dot(direction, up));
}

float3 PTLinear(float3 color) { return pow(max(color, 0.0), 2.2); }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint2 pixel = pt_rect_min + id.xy;
  bool inside = all(pixel < pt_rect_max);
  float4 surface = inside ? pt_gbuffer[pixel] : float4(0.0, 0.0, 0.0, -1.0);
  bool lit = surface.w > 0.0;
  float3 irradiance = float3(1.0, 1.0, 1.0);
  float3 specular = float3(0.0, 0.0, 0.0);

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

  if (lit) {
    float3 up = PTGroundUp(pt_stats.Load3(stats_base));

    // Sun direction relative to the ground plane.
    float3 forward_flat = float3(0.0, 0.0, 1.0) - up * up.z;
    forward_flat = dot(forward_flat, forward_flat) > 1.0e-6 ? normalize(forward_flat)
                                                            : float3(0.0, 0.0, 1.0);
    float3 right_flat = cross(up, forward_flat);
    float elevation = pt_sun_angles.x, azimuth = pt_sun_angles.y;
    float3 sun = up * sin(elevation) +
                 (-forward_flat * cos(azimuth) + right_flat * sin(azimuth)) * cos(elevation);

    float3 normal = surface.xyz;
    float3 position = PTPixelRay(float2(pixel) + 0.5) * surface.w;
    float bias = 2.0e-3 * surface.w + 1.0e-3;
    float3 origin = position + normal * bias;

    // Stratified over 4x4 pixel blocks, which the denoiser then averages.
    uint cell = (pixel.x & 3) | ((pixel.y & 3) << 2);
    uint ray_count = max(pt_ray_count, 1u);
    float sample_count_inv = 1.0 / float(16 * ray_count);

    float3 view = -normalize(position);
    float n_dot_v = max(dot(normal, view), 1.0e-3);
    float alpha = pt_roughness * pt_roughness;
    float alpha2 = alpha * alpha;
    float sun_visibility = 0.0;

    // Direct sun light.
    irradiance = float3(0.0, 0.0, 0.0);
    float n_dot_l = dot(normal, sun);
    if (n_dot_l > 0.0) {
      float3 sun_tangent, sun_bitangent;
      PTBasis(sun, sun_tangent, sun_bitangent);
      uint unshadowed = 0;
      for (uint i = 0; i < ray_count; ++i) {
        uint k = cell + 16 * i;
        float2 u = float2((float(k) + 0.5) * sample_count_inv, PTRadicalInverse(k));
        float phi = 6.28318530718 * u.y;
        float r = sqrt(u.x) * pt_sun_softness;
        float3 direction =
            normalize(sun + sun_tangent * (r * cos(phi)) + sun_bitangent * (r * sin(phi)));
        if (!PTTraceAny(pt_scene, origin, direction, pt_shadow_distance)) {
          ++unshadowed;
        }
      }
      sun_visibility = float(unshadowed) / float(ray_count);
      irradiance += pt_sun_color * (n_dot_l * sun_visibility);
      // Sun highlight.
      if (pt_specular > 0.0) {
        float3 half_vector = normalize(sun + view);
        float n_dot_h = saturate(dot(normal, half_vector));
        float d = n_dot_h * n_dot_h * (alpha2 - 1.0) + 1.0;
        float distribution = alpha2 / (kPTPi * d * d);
        float geometry = PTSmithG1(n_dot_l, alpha2) * PTSmithG1(n_dot_v, alpha2);
        specular += pt_sun_color *
                    (distribution * geometry * PTFresnel(dot(view, half_vector)) /
                     (4.0 * n_dot_v) * sun_visibility * pt_specular);
      }
    }

    // Global illumination.
    float3 tangent, bitangent;
    PTBasis(normal, tangent, bitangent);
    float3 indirect = float3(0.0, 0.0, 0.0);
    bool materials = PTMaterialsValid();
    for (uint i = 0; i < ray_count; ++i) {
      uint k = cell + 16 * i;
      float2 u = float2((float(k) + 0.5) * sample_count_inv, PTRadicalInverse(k));
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
        continue;
      }
      float3 hit = origin + direction * t;
      float3 hit_normal = PTTriangleNormal(pt_vertices, primitive, -direction);
      // Color of the hit surface: its material, or the frame where it's
      // visible if the materials aren't known.
      float3 albedo = float3(0.35, 0.35, 0.35);
      PTSurface hit_material;
      hit_material.valid = false;
      if (materials) {
        hit_material = PTMaterialSurface(primitive, barycentrics);
      }
      if (hit_material.valid) {
        albedo = hit_material.albedo;
      } else if (hit.z > 1.0e-3) {
        int2 hit_pixel = int2(PTProjectToPixel(hit));
        if (PTInRect(hit_pixel)) {
          float4 hit_surface = pt_gbuffer[hit_pixel];
          float3 hit_color = pt_color[hit_pixel].rgb;
          bool seen = hit_surface.w > 0.0 &&
                      abs(hit_surface.w - hit.z) < 0.05 * hit.z + 4.0 * bias;
          albedo = seen ? hit_color : lerp(albedo, hit_color, 0.5);
        }
      }
      // Light the hit surface reflects: the sun if it reaches it, and the
      // sky (roughly half of it visible).
      float3 hit_irradiance = PTSky(sky, up, hit_normal) * 0.5;
      float hit_n_dot_l = dot(hit_normal, sun);
      if (hit_n_dot_l > 0.0) {
        float hit_bias = 2.0e-3 * max(hit.z, 0.0) + 1.0e-3;
        if (!PTTraceAny(pt_scene, hit + hit_normal * hit_bias, sun, pt_shadow_distance)) {
          hit_irradiance += pt_sun_color * hit_n_dot_l;
        }
      }
      indirect += PTLinear(albedo) * hit_irradiance * pt_bounce_scale;
    }
    irradiance += indirect / float(ray_count);

    // Glossy reflection of the scene: one GGX-distributed ray.
    if (pt_specular > 0.0) {
      float2 u = float2((float(cell) + 0.5) * (1.0 / 16.0), PTRadicalInverse(cell));
      // Microfacet normal from the GGX distribution.
      float cos_theta = sqrt((1.0 - u.x) / (1.0 + (alpha2 - 1.0) * u.x));
      float sin_theta = sqrt(max(1.0 - cos_theta * cos_theta, 0.0));
      float phi = 6.28318530718 * u.y;
      float3 half_vector = normalize(tangent * (sin_theta * cos(phi)) +
                                     bitangent * (sin_theta * sin(phi)) + normal * cos_theta);
      float3 direction = reflect(-view, half_vector);
      float n_dot_l = dot(normal, direction);
      if (n_dot_l > 0.0) {
        float v_dot_h = saturate(dot(view, half_vector));
        float n_dot_h = max(dot(normal, half_vector), 1.0e-3);
        // Importance sampled: F * G * (v.h) / (n.h * n.v).
        float weight = PTFresnel(v_dot_h) * PTSmithG1(n_dot_l, alpha2) *
                       PTSmithG1(n_dot_v, alpha2) * v_dot_h / (n_dot_h * n_dot_v);
        float3 radiance;
        float t;
        uint primitive;
        float2 barycentrics;
        if (!PTTraceClosest(pt_scene, origin, direction, 0.0, pt_gi_distance, t, primitive,
                            barycentrics)) {
          radiance = PTSky(sky, up, direction);
        } else {
          float3 hit = origin + direction * t;
          float3 hit_normal = PTTriangleNormal(pt_vertices, primitive, -direction);
          float3 albedo = float3(0.35, 0.35, 0.35);
          if (materials) {
            PTSurface hit_material = PTMaterialSurface(primitive, barycentrics);
            if (hit_material.valid) {
              albedo = hit_material.albedo;
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
          radiance = PTLinear(albedo) * hit_irradiance;
        }
        specular += radiance * (weight * pt_specular);
      }
    }
  }
  if (inside) {
    pt_lighting_out[pixel] = float4(irradiance, 1.0);
    pt_specular_out[pixel] = float4(min(specular, 64.0), 1.0);
  }

  // Average brightness for the auto exposure.
  uint luminance = lit ? uint(min(dot(irradiance, kPTLuminance), 64.0) * 32.0) : 0;
  uint luminance_sum = WaveActiveSum(luminance);
  uint lit_count = WaveActiveCountBits(lit);
  if (WaveIsFirstLane() && lit_count != 0) {
    uint base = pt_stats_slot * kPTStatsSlotSize;
    pt_stats.InterlockedAdd(base + 32, luminance_sum);
    pt_stats.InterlockedAdd(base + 36, lit_count);
  }
}
