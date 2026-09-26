// Tone maps the path traced HDR color with bloom into the final frame.
//
// The lighting is applied to the scene as it was before the HUD was drawn
// (pt_color), and the change is added to the final frame (pt_final_frame)
// where it shows the scene, so the HUD keeps its colors. Without the scene
// image, both are the final frame.

#include "pt_common.hlsli"

// Header of the material buffer: float at 64 - bloom strength.
ByteAddressBuffer pt_materials : register(t7);
RWByteAddressBuffer pt_stats : register(u1);
// Output pixels.
Texture2D<float4> pt_color : register(t0, space3);
Texture2D<float4> pt_final_frame : register(t1, space3);
// Local pixels.
Texture2D<float4> pt_gbuffer : register(t2, space3);
// Exposed linear HDR color, alpha - whether lit by the path tracer.
Texture2D<float4> pt_hdr : register(t3, space3);
Texture2D<float4> pt_albedo : register(t4, space3);
// Quarter resolution bloom of the scene.
Texture2D<float4> pt_bloom : register(t5, space3);
SamplerState pt_sampler_linear_clamp : register(s0);
// Output pixels.
RWTexture2D<float4> pt_output : register(u0, space3);

float3 PTEncode(float3 color) { return pow(saturate(color), 1.0 / 2.2); }

// Narkowicz's ACES filmic curve fit.
float3 PTToneMap(float3 color) {
  return saturate((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  int2 pixel = int2(id.xy);
  if (any(uint2(pixel) >= pt_output_size)) {
    return;
  }
  float4 frame = pt_final_frame[pixel];
  int2 local = pixel - int2(pt_rect_min);
  if (PTInRect(local)) {
    float4 surface = pt_gbuffer[local];
    float3 scene = pt_color[pixel].rgb;
    // The scene as the game drew it into the frame (tinted by fades), and
    // where the HUD covers it.
    float3 tint = asfloat(pt_stats.Load3(kPTStatsTintOffset));
    float3 expected = scene * tint;
    float3 difference = abs(frame.rgb - expected);
    float hud = saturate((max(max(difference.r, difference.g), difference.b) - 0.02) * 40.0);
    float4 hdr = pt_hdr[local];
    uint2 rect_size = PTRectSize();
    float2 bloom_uv = (float2(local) + 0.5) / (float2((rect_size + 3) / 4) * 4.0);
    float3 bloom =
        pt_bloom.SampleLevel(pt_sampler_linear_clamp, bloom_uv, 0.0).rgb *
        asfloat(pt_materials.Load(64));
    float3 result = scene;
    if (pt_debug_view == 1) {
      result = PTEncode(PTToneMap(hdr.rgb));
    } else if (pt_debug_view == 2) {
      result = surface.w > 0.0 ? surface.xyz * 0.5 + 0.5 : float3(0.0, 0.0, 0.0);
    } else if (pt_debug_view == 3) {
      if (surface.w > 0.0) {
        float4 right = pt_gbuffer[min(local + int2(1, 0), int2(rect_size) - 1)];
        float4 below = pt_gbuffer[min(local + int2(0, 1), int2(rect_size) - 1)];
        if (abs(right.w - surface.w) > 0.03 * surface.w ||
            abs(below.w - surface.w) > 0.03 * surface.w) {
          result = float3(1.0, 0.0, 1.0);
        }
      }
      result = lerp(result, float3(0.0, 1.0, 0.0), hud * 0.5);
    } else if (pt_debug_view >= 5 && pt_debug_view <= 7) {
      result = pt_albedo[local].rgb;
    } else if (pt_debug_view != 4 || uint(pixel.x) * 2 >= pt_rect_min.x + pt_rect_max.x) {
      // 4 - the left half of the scene unlit, for comparison.
      if (hdr.a > 0.0) {
        result = lerp(pt_albedo[local].rgb, PTEncode(PTToneMap(hdr.rgb + bloom)), pt_strength);
      } else {
        // The background (sky) keeps its look, with the glow of the scene.
        result = PTEncode(PTLinear(scene) + bloom * pt_strength);
      }
    }
    if (pt_debug_view != 0 && pt_debug_view != 4) {
      frame.rgb = result;
    } else {
      frame.rgb += (result * tint - expected) * (1.0 - hud);
    }
  }
  // The gamma ramp is looked up with the value, like the unorm frame had.
  pt_output[pixel] = saturate(frame);
}
