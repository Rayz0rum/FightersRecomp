// Path traces the lighting of the primary surface of every scene pixel:
// - sun light, with soft shadow rays,
// - global illumination: cosine-distributed rays that either escape to the
//   sky (lit with the background's average color) or hit a surface, which
//   reflects the sun (with its own shadow ray) and the sky with its color taken
//   from the frame where it's visible.
//
// The frame's colors are used as surface albedo, so the result is the ratio
// between the traced irradiance and that of an open ground surface, which the
// frame is then multiplied by.

#include "pt_common.hlsli"

RaytracingAccelerationStructure pt_scene : register(t0);
ByteAddressBuffer pt_vertices : register(t1);
RWByteAddressBuffer pt_stats : register(u1);
Texture2D<float4> pt_color : register(t2);
Texture2D<float4> pt_gbuffer : register(t3);
RWTexture2D<float4> pt_lighting_out : register(u2);

bool PTTraceClosest(float3 origin, float3 direction, float t_max, out float t, out uint primitive) {
  RayDesc ray;
  ray.Origin = origin;
  ray.Direction = direction;
  ray.TMin = 0.0;
  ray.TMax = t_max;
  RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
  query.TraceRayInline(pt_scene, RAY_FLAG_NONE, 0xFF, ray);
  query.Proceed();
  t = query.CommittedRayT();
  primitive = query.CommittedPrimitiveIndex();
  return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

bool PTTraceAny(float3 origin, float3 direction, float t_max) {
  RayDesc ray;
  ray.Origin = origin;
  ray.Direction = direction;
  ray.TMin = 0.0;
  ray.TMax = t_max;
  RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
  query.TraceRayInline(pt_scene, RAY_FLAG_NONE, 0xFF, ray);
  query.Proceed();
  return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

float3 PTTriangleNormal(uint primitive, float3 towards) {
  uint address = primitive * 36;
  float3 p0 = asfloat(pt_vertices.Load3(address));
  float3 p1 = asfloat(pt_vertices.Load3(address + 12));
  float3 p2 = asfloat(pt_vertices.Load3(address + 24));
  float3 normal = normalize(cross(p1 - p0, p2 - p0));
  return dot(normal, towards) < 0.0 ? -normal : normal;
}

// Radiance of the sky in a direction.
float3 PTSky(float3 sky, float3 up, float3 direction) {
  return sky * (0.6 + 0.4 * dot(direction, up));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint2 pixel = pt_rect_min + id.xy;
  bool inside = all(pixel < pt_rect_max);
  float4 surface = inside ? pt_gbuffer[pixel] : float4(0.0, 0.0, 0.0, -1.0);
  bool lit = surface.w > 0.0;
  float3 ratio = float3(1.0, 1.0, 1.0);

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
  sky = sky * pt_sky_scale + pt_ambient;

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

    // Direct sun light, with as many soft shadow rays as for the global
    // illumination, over the sun's disk.
    float3 irradiance = float3(0.0, 0.0, 0.0);
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
        if (!PTTraceAny(origin, direction, pt_shadow_distance)) {
          ++unshadowed;
        }
      }
      irradiance += pt_sun_color * (n_dot_l * float(unshadowed) / float(ray_count));
    }

    // Global illumination.
    float3 tangent, bitangent;
    PTBasis(normal, tangent, bitangent);
    float3 indirect = float3(0.0, 0.0, 0.0);
    for (uint i = 0; i < ray_count; ++i) {
      uint k = cell + 16 * i;
      float2 u = float2((float(k) + 0.5) * sample_count_inv, PTRadicalInverse(k));
      float phi = 6.28318530718 * u.y;
      float r = sqrt(u.x);
      float3 direction = tangent * (r * cos(phi)) + bitangent * (r * sin(phi)) +
                         normal * sqrt(max(1.0 - u.x, 0.0));
      float t;
      uint primitive;
      if (!PTTraceClosest(origin, direction, pt_gi_distance, t, primitive)) {
        indirect += PTSky(sky, up, direction);
        continue;
      }
      float3 hit = origin + direction * t;
      float3 hit_normal = PTTriangleNormal(primitive, -direction);
      // Albedo of the hit surface from the frame, if it's visible there.
      float3 albedo = float3(0.35, 0.35, 0.35);
      if (hit.z > 1.0e-3) {
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
        if (!PTTraceAny(hit + hit_normal * hit_bias, sun, pt_shadow_distance)) {
          hit_irradiance += pt_sun_color * hit_n_dot_l;
        }
      }
      indirect += albedo * hit_irradiance * pt_bounce_scale;
    }
    irradiance += indirect / float(ray_count);

    // Irradiance of open ground: sun at its elevation and the whole sky
    // (0.6 + 0.4 * 2/3 on average over the cosine-weighted hemisphere).
    float open = dot(pt_sun_color, kPTLuminance) * max(sin(elevation), 0.0) +
                 dot(sky, kPTLuminance) * (0.6 + 0.4 * 2.0 / 3.0);
    ratio = irradiance / max(open, 1.0e-3);
  }
  if (inside) {
    pt_lighting_out[pixel] = float4(ratio, 1.0);
  }

  // Average brightness for the auto exposure.
  uint luminance = lit ? uint(min(dot(ratio, kPTLuminance), 8.0) * 256.0) : 0;
  uint luminance_sum = WaveActiveSum(luminance);
  uint lit_count = WaveActiveCountBits(lit);
  if (WaveIsFirstLane() && lit_count != 0) {
    uint base = pt_stats_slot * kPTStatsSlotSize;
    pt_stats.InterlockedAdd(base + 32, luminance_sum);
    pt_stats.InterlockedAdd(base + 36, lit_count);
  }
}
