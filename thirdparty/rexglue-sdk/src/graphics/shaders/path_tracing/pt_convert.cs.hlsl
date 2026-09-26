// Turns the stream output capture (host clip space positions and material
// attributes, one triangle per 3 vertices) into view space triangles for the
// acceleration structure and per-triangle material attributes, and estimates
// the ground plane orientation from the up-facing triangles.
//
// Solid geometry only - blended draws (effects) stay out, and so do the
// game's own shadows (ground polygons colored black by their color table),
// which the traced sun replaces.

#include "pt_material.hlsli"

ByteAddressBuffer pt_capture : register(t0);
// UINT64 filled size of each draw's region of the capture buffer.
ByteAddressBuffer pt_capture_counts : register(t1);
RWByteAddressBuffer pt_vertices : register(u0);
RWByteAddressBuffer pt_stats : register(u1);
RWByteAddressBuffer pt_attributes_out : register(u4);

// The draw that wrote a triangle (draws are in capture order).
uint PTFindDraw(uint triangle_index, uint draw_count) {
  uint low = 0, high = draw_count;
  while (high - low > 1) {
    uint middle = (low + high) >> 1;
    if (pt_materials.Load(kPTMaterialDrawsOffset + middle * 32) <= triangle_index) {
      low = middle;
    } else {
      high = middle;
    }
  }
  return low;
}

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
  // The draw that wrote the triangle, and whether it did (its region is sized
  // for the most triangles it could have written).
  uint draw_index = 0xFFFFFFFFu;
  uint draw_flags = 0;
  bool valid = false;
  uint draw_count = pt_materials.Load(68);
  if (draw_count != 0) {
    draw_index = PTFindDraw(triangle_index, draw_count);
    uint4 draw = PTDraw(draw_index);
    uint written = pt_capture_counts.Load(draw_index * 8) / 96;
    valid = triangle_index >= draw.x && triangle_index < draw.x + min(written, draw.y);
    draw_flags = draw.w;
  }
  float3 p[3];
  float2 uv[3];
  float row = 0.0;
  float3 light_factors;
  [unroll] for (uint i = 0; i < 3; ++i) {
    uint address = (triangle_index * 3 + i) * 32;
    float4 clip = asfloat(pt_capture.Load4(address));
    float4 material = asfloat(pt_capture.Load4(address + 16));
    float2 guest_clip = (clip.xy - pt_ndc_offset * clip.w) / pt_ndc_scale;
    // Linear in the clip position, so vertices behind the camera are still
    // placed correctly.
    p[i] = float3(guest_clip * pt_inv_proj, clip.w);
    uv[i] = material.xy;
    light_factors[i] = material.w;
    if (i == 0) {
      row = material.z;
    }
    valid = valid && all(abs(p[i]) < 1.0e6);
  }

  // Material attributes, for every triangle.
  uint attribute_address = triangle_index * 48;
  pt_attributes_out.Store4(attribute_address,
                           uint4(asuint(uv[0]), asuint(uv[1])));
  pt_attributes_out.Store4(attribute_address + 16,
                           uint4(asuint(uv[2]), asuint(row), draw_index));
  pt_attributes_out.Store4(attribute_address + 32, uint4(asuint(light_factors), 0));
  valid = valid && (draw_flags & kPTDrawOpaque);

  float3 normal = cross(p[1] - p[0], p[2] - p[0]);
  float double_area = length(normal);
  valid = valid && double_area > 1.0e-12;
  if (valid) {
    float3 center = (p[0] + p[1] + p[2]) * (1.0 / 3.0);
    normal /= double_area;
    if (dot(normal, center) > 0.0) {
      normal = -normal;
    }
    // The game's own shadows: black, lying on the ground (facing up in view
    // space, as the camera looks at the ground from above).
    if ((draw_flags & kPTDrawMaterial) && PTMaterialsValid() && normal.y > 0.5 &&
        (pt_flags & kPTFlagReplaceGameShadows)) {
      PTSurface surface =
          PTMaterialSurfaceAt((uv[0] + uv[1] + uv[2]) * (1.0 / 3.0), row, draw_index);
      if (surface.valid && max(max(surface.albedo.r, surface.albedo.g), surface.albedo.b) < 0.02) {
        valid = false;
      }
    }
  }
  if (valid) {
    float3 center = (p[0] + p[1] + p[2]) * (1.0 / 3.0);
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
  // Solid triangles into the opaque geometry, alpha-tested ones into the
  // other, inactive (NaN) in the one they're not in.
  float nan = asfloat(0x7FC00000u);
  float3 inactive = float3(nan, 0.0, 0.0);
  bool alpha_tested = (draw_flags & kPTDrawAlphaTest) && PTMaterialsValid();
  uint address = triangle_index * 36;
  uint address_opaque = address, address_alpha_tested = address + kPTVertexRegionSize;
  uint address_used = alpha_tested ? address_alpha_tested : address_opaque;
  uint address_unused = alpha_tested ? address_opaque : address_alpha_tested;
  pt_vertices.Store3(address_used, asuint(p[0]));
  pt_vertices.Store3(address_used + 12, asuint(p[1]));
  pt_vertices.Store3(address_used + 24, asuint(p[2]));
  pt_vertices.Store3(address_unused, asuint(inactive));
  pt_vertices.Store3(address_unused + 12, asuint(inactive));
  pt_vertices.Store3(address_unused + 24, asuint(inactive));
}
