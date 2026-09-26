// Turns the stream output capture (host clip space positions, one triangle
// per 3 vertices) into view space triangles for the acceleration structure,
// and estimates the ground plane orientation from the up-facing triangles.

#include "pt_common.hlsli"

ByteAddressBuffer pt_capture : register(t0);
ByteAddressBuffer pt_capture_filled : register(t1);
RWByteAddressBuffer pt_vertices : register(u0);
RWByteAddressBuffer pt_stats : register(u1);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x == 0) {
    // Nothing reads the other half during this frame - prepare it for the
    // next one.
    uint clear_base = (pt_stats_slot ^ 1) * kPTStatsSlotSize;
    [unroll] for (uint i = 0; i < kPTStatsSlotSize; i += 16) {
      pt_stats.Store4(clear_base + i, uint4(0, 0, 0, 0));
    }
  }
  uint triangle_index = id.x;
  if (triangle_index * 3 >= pt_vertex_count) {
    return;
  }
  uint captured_triangles = pt_capture_filled.Load(0) / 48;
  float3 p[3];
  bool valid = triangle_index < captured_triangles;
  [unroll] for (uint i = 0; i < 3; ++i) {
    float4 clip = asfloat(pt_capture.Load4((triangle_index * 3 + i) * 16));
    float2 guest_clip = (clip.xy - pt_ndc_offset * clip.w) / pt_ndc_scale;
    // Linear in the clip position, so vertices behind the camera are still
    // placed correctly.
    p[i] = float3(guest_clip * pt_inv_proj, clip.w);
    valid = valid && all(abs(p[i]) < 1.0e6);
  }
  float3 normal = cross(p[1] - p[0], p[2] - p[0]);
  float double_area = length(normal);
  valid = valid && double_area > 1.0e-12;
  if (valid) {
    float3 center = (p[0] + p[1] + p[2]) * (1.0 / 3.0);
    normal /= double_area;
    if (dot(normal, center) > 0.0) {
      normal = -normal;
    }
    // Weighted by the solid angle, so what's actually seen dominates.
    if (normal.y > 0.6 && center.z > 0.0) {
      float weight = min(0.5 * double_area / max(center.z * center.z, 1.0e-4), 4.0);
      int3 contribution = int3(normal * (weight * kPTStatsScale));
      uint base = pt_stats_slot * kPTStatsSlotSize;
      pt_stats.InterlockedAdd(base + 0, asuint(contribution.x));
      pt_stats.InterlockedAdd(base + 4, asuint(contribution.y));
      pt_stats.InterlockedAdd(base + 8, asuint(contribution.z));
    }
  } else {
    // NaN x on every vertex makes the triangle inactive.
    float nan = asfloat(0x7FC00000u);
    p[0] = float3(nan, 0.0, 0.0);
    p[1] = p[0];
    p[2] = p[0];
  }
  uint address = triangle_index * 36;
  pt_vertices.Store3(address, asuint(p[0]));
  pt_vertices.Store3(address + 12, asuint(p[1]));
  pt_vertices.Store3(address + 24, asuint(p[2]));
}
