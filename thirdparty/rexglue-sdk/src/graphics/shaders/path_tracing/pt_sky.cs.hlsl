// Learns the sky around the scene in world space: an octahedral map of the
// background's color by direction, updated from wherever the camera sees the
// background (sky, clouds, far scenery), so light escaping the scene has the
// color of the sky in its direction - stable whichever way the camera looks.
//
// One thread per map texel: its direction (jittered within the texel every
// frame) is projected into the frame, and if the background is visible there,
// its color is blended in. Alpha is how much of the texel has been seen.

#include "pt_common.hlsli"

// Output pixels.
Texture2D<float4> pt_color : register(t0, space3);
// Local pixels.
Texture2D<float4> pt_gbuffer : register(t1, space3);
RWTexture2D<float4> pt_sky_map : register(u0, space3);

static const uint kPTSkyMapSize = 32;

float3 PTOctahedronToDirection(float2 uv) {
  uv = uv * 2.0 - 1.0;
  float3 n = float3(uv, 1.0 - abs(uv.x) - abs(uv.y));
  float t = saturate(-n.z);
  n.xy += float2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
  return normalize(n);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= kPTSkyMapSize)) {
    return;
  }
  float4 texel = pt_world_reset ? float4(0.0, 0.0, 0.0, 0.0) : pt_sky_map[id.xy];
  uint seed = PTRandomSeed(id.xy, 3);
  float2 uv = (float2(id.xy) + PTRandom2(seed)) / float(kPTSkyMapSize);
  float3 direction = PTWorldToView(PTOctahedronToDirection(uv));
  if (direction.z > 0.05) {
    float2 pixel = PTProjectToPixel(direction / direction.z);
    int2 local = int2(floor(pixel)) - int2(pt_rect_min);
    if (PTInRect(local) && pt_gbuffer[local].w < 0.0) {
      float3 color = PTLinear(pt_color[pt_rect_min + uint2(local)].rgb);
      texel.rgb = texel.a > 0.0 ? lerp(texel.rgb, color, 0.08) : color;
      texel.a = min(texel.a + 0.05, 1.0);
    }
  }
  pt_sky_map[id.xy] = texel;
}
