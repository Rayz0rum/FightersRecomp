// Surface materials of the captured triangles, and ray queries honoring
// their alpha test.
//
// The material model is the game's pixel shader (see
// path_tracing_albedo_shader): the texture holds intensities, scaled by the
// per-vertex lighting factor, and a row of a color table (Model 2 style) maps
// them to colors. The surface color is the game's at its own lighting factor
// (some rows aren't plain brightness ramps - the factor picks the colors),
// with the brightness then divided by the factor.

#ifndef PT_MATERIAL_HLSLI_
#define PT_MATERIAL_HLSLI_

#include "pt_common.hlsli"

// Header: float4 c254, c255, c1, c0 (the shader's constants), uint4 (0, draw
// count, whether the materials are valid, 0). Then per draw: uint4 (first
// triangle, triangle count, texture descriptor, flags), uint4 (color table
// descriptor, alpha test scale and bias, the draw's first triangle in the
// previous frame or 0xFFFFFFFF).
ByteAddressBuffer pt_materials : register(t7);
// Per triangle: float2 texture coordinates x3, float color table row, uint
// draw index, float3 the game's lighting factors of the vertices, 0.
ByteAddressBuffer pt_attributes : register(t8);
Texture2D<float4> pt_material_textures[] : register(t0, space1);
SamplerState pt_sampler_linear_clamp : register(s0);

static const uint kPTMaterialDrawsOffset = 80;
static const uint kPTDrawOpaque = 1u << 0;
static const uint kPTDrawTransparent = 1u << 1;
static const uint kPTDrawMaterial = 1u << 2;
static const uint kPTDrawAlphaTest = 1u << 3;
// Offset of the alpha-tested triangles in the vertex buffer.
static const uint kPTVertexRegionSize = 131072 * 3 * 12;

bool PTMaterialsValid() { return pt_materials.Load(72) != 0; }

uint4 PTDraw(uint draw_index) {
  return pt_materials.Load4(kPTMaterialDrawsOffset + draw_index * 32);
}

uint4 PTDrawMaterial(uint draw_index) {
  return pt_materials.Load4(kPTMaterialDrawsOffset + draw_index * 32 + 16);
}

struct PTSurface {
  float3 albedo;
  // Passes the alpha test if >= 0.
  float alpha_margin;
  bool valid;
};

// Undoes the game's lighting of a color it rendered with a lighting factor:
// the color table value (before the shader's output transform) divided by
// the factor.
float3 PTNormalizeLighting(float3 color, float factor) {
  float4 c254 = asfloat(pt_materials.Load4(0));
  float4 c255 = asfloat(pt_materials.Load4(16));
  float exponent = max(asfloat(pt_materials.Load(48)), 1.0e-3);
  float3 value = (pow(saturate(color), 1.0 / exponent) - c255.x) / max(c254.w, 1.0e-3);
  value /= max(factor, 0.25);
  return pow(saturate(value * c254.w + c255.x), exponent);
}

PTSurface PTMaterialSurfaceAt(float2 uv, float row, uint draw_index, float factor) {
  PTSurface surface;
  surface.albedo = float3(0.35, 0.35, 0.35);
  surface.alpha_margin = 1.0;
  surface.valid = false;
  if (draw_index == 0xFFFFFFFFu) {
    return surface;
  }
  uint4 draw = PTDraw(draw_index);
  uint4 draw_material = PTDrawMaterial(draw_index);
  if (!(draw.w & kPTDrawMaterial) || draw.z == 0xFFFFFFFFu ||
      draw_material.x == 0xFFFFFFFFu) {
    return surface;
  }
  Texture2D<float4> texture = pt_material_textures[NonUniformResourceIndex(draw.z)];
  uint width, height;
  texture.GetDimensions(width, height);
  float4 texel = texture.Load(
      int3(int2(floor(frac(uv) * float2(width, height))) % int2(max(width, 1u), max(height, 1u)),
           0));
  float4 c254 = asfloat(pt_materials.Load4(0));
  float4 c255 = asfloat(pt_materials.Load4(16));
  float4 c0 = asfloat(pt_materials.Load4(48));
  // Color table column from the intensity at the game's lighting factor, and
  // the value normalized for the factor.
  float column = texel.x * c254.x * factor + c254.z;
  float4 color = pt_material_textures[NonUniformResourceIndex(draw_material.x)].SampleLevel(
      pt_sampler_linear_clamp, float2(column, row), 0.0);
  surface.albedo =
      pow(saturate(color.xyz / max(factor, 0.25) * c254.w + c255.x), max(c0.x, 1.0e-3));
  surface.alpha_margin = texel.w * asfloat(draw_material.y) - asfloat(draw_material.z);
  surface.valid = true;
  return surface;
}

PTSurface PTMaterialSurface(uint triangle_index, float2 barycentrics) {
  uint address = triangle_index * 48;
  uint4 attributes_0 = pt_attributes.Load4(address);
  uint4 attributes_1 = pt_attributes.Load4(address + 16);
  float3 weights = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
  float2 uv = asfloat(attributes_0.xy) * weights.x + asfloat(attributes_0.zw) * weights.y +
              asfloat(attributes_1.xy) * weights.z;
  float factor = dot(asfloat(pt_attributes.Load3(address + 32)), weights);
  return PTMaterialSurfaceAt(uv, asfloat(attributes_1.z), attributes_1.w, factor);
}

// The game's own lighting factors of the triangle's vertices.
float3 PTLightFactors(uint triangle_index) {
  return asfloat(pt_attributes.Load3(triangle_index * 48 + 32));
}

bool PTMaterialAlphaPasses(uint triangle_index, float2 barycentrics) {
  if (!PTMaterialsValid()) {
    return true;
  }
  return PTMaterialSurface(triangle_index, barycentrics).alpha_margin >= 0.0;
}

// Closest hit with alpha-tested geometry.
bool PTTraceClosest(RaytracingAccelerationStructure scene, float3 origin, float3 direction,
                    float t_min, float t_max, out float t, out uint primitive,
                    out float2 barycentrics) {
  RayDesc ray;
  ray.Origin = origin;
  ray.Direction = direction;
  ray.TMin = t_min;
  ray.TMax = t_max;
  RayQuery<RAY_FLAG_NONE> query;
  query.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
  while (query.Proceed()) {
    if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
        PTMaterialAlphaPasses(query.CandidatePrimitiveIndex(),
                              query.CandidateTriangleBarycentrics())) {
      query.CommitNonOpaqueTriangleHit();
    }
  }
  t = query.CommittedRayT();
  primitive = query.CommittedPrimitiveIndex();
  barycentrics = query.CommittedTriangleBarycentrics();
  return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// Any hit (for shadows) with alpha-tested geometry.
bool PTTraceAny(RaytracingAccelerationStructure scene, float3 origin, float3 direction,
                float t_max) {
  RayDesc ray;
  ray.Origin = origin;
  ray.Direction = direction;
  ray.TMin = 0.0;
  ray.TMax = t_max;
  RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
  query.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
  while (query.Proceed()) {
    if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
        PTMaterialAlphaPasses(query.CandidatePrimitiveIndex(),
                              query.CandidateTriangleBarycentrics())) {
      query.CommitNonOpaqueTriangleHit();
    }
  }
  return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

float3 PTTriangleNormal(ByteAddressBuffer vertices, uint primitive, float3 towards) {
  uint address = primitive * 36;
  // Solid or alpha-tested (the other region has NaN).
  if (isnan(asfloat(vertices.Load(address)))) {
    address += kPTVertexRegionSize;
  }
  float3 p0 = asfloat(vertices.Load3(address));
  float3 p1 = asfloat(vertices.Load3(address + 12));
  float3 p2 = asfloat(vertices.Load3(address + 24));
  float3 normal = normalize(cross(p1 - p0, p2 - p0));
  return dot(normal, towards) < 0.0 ? -normal : normal;
}

#endif  // PT_MATERIAL_HLSLI_
