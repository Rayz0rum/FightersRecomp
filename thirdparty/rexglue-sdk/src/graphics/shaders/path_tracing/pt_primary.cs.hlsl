// Primary visibility: traces a camera ray through every pixel of the scene to
// get the surface there (geometric normal and view depth). Pixels without
// geometry (or only far scenery) show the background, whose average color
// lights the scene as the sky.

#include "pt_common.hlsli"

RaytracingAccelerationStructure pt_scene : register(t0);
ByteAddressBuffer pt_vertices : register(t1);
RWByteAddressBuffer pt_stats : register(u1);
Texture2D<float4> pt_color : register(t2);
RWTexture2D<float4> pt_gbuffer_out : register(u2);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint2 pixel = pt_rect_min + id.xy;
  bool inside = all(pixel < pt_rect_max);
  float4 result = float4(0.0, 0.0, 0.0, -1.0);
  if (inside) {
    float3 direction = PTPixelRay(float2(pixel) + 0.5);
    RayDesc ray;
    ray.Origin = float3(0.0, 0.0, 0.0);
    ray.Direction = direction;
    ray.TMin = 1.0e-3;
    ray.TMax = pt_max_distance;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(pt_scene, RAY_FLAG_NONE, 0xFF, ray);
    query.Proceed();
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
      uint address = query.CommittedPrimitiveIndex() * 36;
      float3 p0 = asfloat(pt_vertices.Load3(address));
      float3 p1 = asfloat(pt_vertices.Load3(address + 12));
      float3 p2 = asfloat(pt_vertices.Load3(address + 24));
      float3 normal = normalize(cross(p1 - p0, p2 - p0));
      if (dot(normal, direction) > 0.0) {
        normal = -normal;
      }
      // The direction has z = 1, so the distance is the view depth.
      result = float4(normal, query.CommittedRayT());
    }
    pt_gbuffer_out[pixel] = result;
  }

  // Background color statistics.
  bool background = inside && result.w < 0.0;
  uint3 color = background ? uint3(saturate(pt_color[pixel].rgb) * 255.0 + 0.5) : uint3(0, 0, 0);
  uint3 color_sum = WaveActiveSum(color);
  uint count = WaveActiveCountBits(background);
  if (WaveIsFirstLane() && count != 0) {
    uint base = pt_stats_slot * kPTStatsSlotSize;
    pt_stats.InterlockedAdd(base + 16, color_sum.r);
    pt_stats.InterlockedAdd(base + 20, color_sum.g);
    pt_stats.InterlockedAdd(base + 24, color_sum.b);
    pt_stats.InterlockedAdd(base + 28, count);
  }
}
