// Finds the direction of the game's own light in world space, so the traced
// sun comes from where the game lights the scene from - fixed in the world,
// consistent across camera angles and cuts.
//
// The game lights every polygon with a single factor from its normal and a
// directional light (Model 2 style, about clamp(a + b * max(n.l, 0))). The
// light is the direction whose max(n.l, 0) correlates best with the
// polygons' factors. The candidates are the previous frame's estimate with
// rings around it and directions spread over the sphere, and the estimate is
// smoothed over frames (heavily - it's fixed in the world).
//
// One group; the triangles are shared through group memory in tiles.

#include "pt_material.hlsli"

ByteAddressBuffer pt_vertices : register(t1);
RWByteAddressBuffer pt_stats : register(u1);

static const uint kPTSunCandidates = 64;
static const uint kPTSunLocalCandidates = 25;

groupshared float4 pt_sun_triangles[kPTSunCandidates];
groupshared float pt_sun_scores[kPTSunCandidates];
groupshared float3 pt_sun_directions[kPTSunCandidates];

float3 PTSunCandidate(uint index, float3 previous, uint frame) {
  if (index < kPTSunLocalCandidates) {
    if (index == 0) {
      return previous;
    }
    // Rings of 8 at increasing distances.
    uint ring = (index - 1) >> 3;
    float angle = (float((index - 1) & 7) + 0.5 * float(ring)) * (6.28318530718 / 8.0);
    float distance = ring == 0 ? 0.015 : (ring == 1 ? 0.06 : 0.2);
    float3 t, b;
    PTBasis(previous, t, b);
    return normalize(previous + (t * cos(angle) + b * sin(angle)) * distance);
  }
  // Fibonacci sphere, rotated every frame to cover it over time.
  uint count = kPTSunCandidates - kPTSunLocalCandidates;
  float i = float(index - kPTSunLocalCandidates) + 0.5;
  float cos_phi = 1.0 - 2.0 * i / float(count);
  float sin_phi = sqrt(max(1.0 - cos_phi * cos_phi, 0.0));
  float theta = 2.39996322973 * i + float(frame & 255) * 0.61803398875 * 6.28318530718;
  return float3(cos(theta) * sin_phi, cos_phi, sin(theta) * sin_phi);
}

[numthreads(64, 1, 1)]
void main(uint3 thread : SV_GroupThreadID) {
  uint lane = thread.x;
  uint state_read = kPTStatsSunOffset + (pt_stats_slot ^ 1) * 16;
  uint state_write = kPTStatsSunOffset + pt_stats_slot * 16;
  float4 state = asfloat(pt_stats.Load4(state_read));
  bool state_valid = !pt_world_reset && state.w > 0.0 && all(abs(state.xyz) <= 1.0) &&
                     dot(state.xyz, state.xyz) > 0.5;
  float3 previous = state_valid ? normalize(state.xyz) : normalize(float3(-0.5, 0.8, -0.3));
  float3 candidate = PTSunCandidate(lane, previous, pt_frame_index);
  pt_sun_directions[lane] = candidate;

  // Sums over the polygons for this lane's candidate x = max(n.l, 0), and the
  // factors f.
  float sum_x = 0.0, sum_xx = 0.0, sum_xf = 0.0;
  float sum_f = 0.0, sum_ff = 0.0, count = 0.0;
  uint triangle_count = pt_vertex_count / 3;
  bool materials = PTMaterialsValid();
  for (uint tile = 0; tile < triangle_count; tile += kPTSunCandidates) {
    // Each lane loads a triangle of the tile.
    float4 entry = float4(0.0, 0.0, 0.0, -1.0);
    uint triangle_index = tile + lane;
    if (triangle_index < triangle_count && materials) {
      uint address = triangle_index * 36;
      if (isnan(asfloat(pt_vertices.Load(address)))) {
        address += kPTVertexRegionSize;
      }
      float3 p0 = asfloat(pt_vertices.Load3(address));
      if (!isnan(p0.x)) {
        float3 p1 = asfloat(pt_vertices.Load3(address + 12));
        float3 p2 = asfloat(pt_vertices.Load3(address + 24));
        float3 center = (p0 + p1 + p2) * (1.0 / 3.0);
        float3 normal = cross(p1 - p0, p2 - p0);
        float normal_length = length(normal);
        uint draw_index = pt_attributes.Load(triangle_index * 48 + 28);
        // Scene polygons with materials (the game's lighting factor), not the
        // distant scenery.
        if (normal_length > 1.0e-12 && center.z > 0.0 && center.z < pt_max_distance &&
            draw_index != 0xFFFFFFFFu && (PTDraw(draw_index).w & kPTDrawMaterial)) {
          normal /= normal_length;
          if (dot(normal, center) > 0.0) {
            normal = -normal;
          }
          entry = float4(PTViewToWorld(normal), PTLightFactors(triangle_index).x);
        }
      }
    }
    pt_sun_triangles[lane] = entry;
    GroupMemoryBarrierWithGroupSync();
    [loop] for (uint i = 0; i < kPTSunCandidates; ++i) {
      float4 polygon = pt_sun_triangles[i];
      if (polygon.w < 0.0) {
        continue;
      }
      float x = max(dot(polygon.xyz, candidate), 0.0);
      sum_x += x;
      sum_xx += x * x;
      sum_xf += x * polygon.w;
      sum_f += polygon.w;
      sum_ff += polygon.w * polygon.w;
      count += 1.0;
    }
    GroupMemoryBarrierWithGroupSync();
  }

  // Correlation of the factors with the candidate's lighting.
  float score = -2.0;
  if (count >= 16.0) {
    float mean_x = sum_x / count, mean_f = sum_f / count;
    float variance_x = sum_xx / count - mean_x * mean_x;
    float variance_f = sum_ff / count - mean_f * mean_f;
    float covariance = sum_xf / count - mean_x * mean_f;
    if (variance_x > 1.0e-6 && variance_f > 1.0e-6) {
      score = covariance * rsqrt(variance_x * variance_f);
    }
  }
  pt_sun_scores[lane] = score;
  GroupMemoryBarrierWithGroupSync();

  if (lane == 0) {
    uint best = 0;
    for (uint i = 1; i < kPTSunCandidates; ++i) {
      if (pt_sun_scores[i] > pt_sun_scores[best]) {
        best = i;
      }
    }
    float best_score = pt_sun_scores[best];
    float4 result = state_valid ? float4(previous, state.w) : float4(0.0, 0.0, 0.0, 0.0);
    // Only with a clear dependency on the direction (lit scenes).
    if (best_score > 0.3) {
      float3 direction = pt_sun_directions[best];
      if (!state_valid ||
          (dot(direction, previous) < 0.9 && best_score > pt_sun_scores[0] + 0.1)) {
        // First estimate, or clearly wrong so far.
        result = float4(direction, best_score);
      } else {
        result = float4(normalize(lerp(previous, direction, 0.08)),
                        lerp(state.w, best_score, 0.08));
      }
    }
    pt_stats.Store4(state_write, asuint(result));
  }
}
