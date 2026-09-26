// Bloom at a quarter of the resolution of the scene: pt_filter_step selects
// the pass - 0 downsamples the bright part of the HDR color (above the
// threshold), 1 and 2 blur horizontally and vertically.

#include "pt_common.hlsli"

// Header of the material buffer: float at 64 - bloom strength, at 76 -
// threshold (the rest isn't used here).
ByteAddressBuffer pt_materials : register(t7);
Texture2D<float4> pt_source : register(t4);
RWTexture2D<float4> pt_bloom_out : register(u2);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint2 rect_size = pt_rect_max - pt_rect_min;
  uint2 bloom_size = (rect_size + 3) / 4;
  if (any(id.xy >= bloom_size)) {
    return;
  }
  if (pt_filter_step == 0) {
    float threshold = asfloat(pt_materials.Load(76));
    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll] for (uint y = 0; y < 4; ++y) {
      [unroll] for (uint x = 0; x < 4; ++x) {
        uint2 pixel = min(pt_rect_min + id.xy * 4 + uint2(x, y), pt_rect_max - 1);
        float4 hdr = pt_source[pixel];
        sum += max(hdr.rgb - threshold, 0.0) * hdr.a;
      }
    }
    pt_bloom_out[id.xy] = float4(sum * (1.0 / 16.0), 1.0);
    return;
  }
  int2 axis = pt_filter_step == 1 ? int2(1, 0) : int2(0, 1);
  float3 sum = float3(0.0, 0.0, 0.0);
  float weight_sum = 0.0;
  for (int i = -12; i <= 12; ++i) {
    int2 tap = clamp(int2(id.xy) + axis * i, int2(0, 0), int2(bloom_size) - 1);
    float weight = exp(-float(i * i) * (1.0 / 50.0));
    sum += pt_source[tap].rgb * weight;
    weight_sum += weight;
  }
  pt_bloom_out[id.xy] = float4(sum / weight_sum, 1.0);
}
