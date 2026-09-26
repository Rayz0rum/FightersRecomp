// Applies the denoised path-traced lighting to the frame, with auto exposure
// keeping the average brightness near the original's.
//
// The lighting is applied to the scene as it was before the HUD was drawn
// (pt_color), and the change is added to the final frame (pt_final_frame) where it
// shows the scene, so the HUD keeps its colors. Without the scene image, both
// are the final frame.

#include "pt_common.hlsli"

RWByteAddressBuffer pt_stats : register(u1);
Texture2D<float4> pt_color : register(t2);
Texture2D<float4> pt_gbuffer : register(t3);
Texture2D<float4> pt_lighting : register(t4);
Texture2D<float4> pt_final_frame : register(t5);
// The scene with the game's own shadows filled in.
Texture2D<float4> pt_albedo : register(t6);
RWTexture2D<float4> pt_output : register(u3);

// Keeps some detail in the highlights instead of clipping.
float3 PTShoulder(float3 color) {
  const float knee = 0.8;
  float3 over = max(color - knee, 0.0);
  return min(color, knee) + over / (1.0 + over * (1.0 / (1.0 - knee)));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  int2 pixel = int2(id.xy);
  if (any(uint2(pixel) >= pt_output_size)) {
    return;
  }
  float4 frame = pt_final_frame[pixel];
  if (PTInRect(pixel)) {
    float4 surface = pt_gbuffer[pixel];
    float3 scene = pt_color[pixel].rgb;
    // Where the HUD covers the scene.
    float3 difference = abs(frame.rgb - scene);
    float hud = saturate((max(max(difference.r, difference.g), difference.b) - 0.03) * 16.0);
    if (surface.w > 0.0) {
      uint2 average = pt_stats.Load2(pt_stats_slot * kPTStatsSlotSize + 32);
      float average_luminance =
          average.y != 0 ? float(average.x) / (256.0 * float(average.y)) : 1.0;
      float exposure = clamp(pt_exposure_target / max(average_luminance, 1.0e-3), 0.75, 2.0);
      float3 lighting = pt_lighting[pixel].rgb * exposure;
      if (pt_debug_view == 1) {
        frame.rgb = lighting * 0.5;
      } else if (pt_debug_view == 2) {
        frame.rgb = surface.xyz * 0.5 + 0.5;
      } else if (pt_debug_view == 3) {
        float4 right = pt_gbuffer[min(pixel + int2(1, 0), int2(pt_rect_max) - 1)];
        float4 below = pt_gbuffer[min(pixel + int2(0, 1), int2(pt_rect_max) - 1)];
        if (abs(right.w - surface.w) > 0.03 * surface.w ||
            abs(below.w - surface.w) > 0.03 * surface.w) {
          frame.rgb = float3(1.0, 0.0, 1.0);
        }
        frame.rgb = lerp(frame.rgb, float3(0.0, 1.0, 0.0), hud * 0.5);
      } else if (pt_debug_view == 5) {
        frame.rgb = pt_albedo[pixel].rgb;
      } else if (pt_debug_view != 4 || uint(pixel.x) * 2 >= pt_rect_min.x + pt_rect_max.x) {
        // 4 - the left half of the scene without path tracing, for comparison.
        float3 relit = PTShoulder(pt_albedo[pixel].rgb *
                                  lerp(float3(1.0, 1.0, 1.0), lighting, pt_strength));
        frame.rgb += (relit - scene) * (1.0 - hud);
      }
    } else if (pt_debug_view == 1 || pt_debug_view == 2) {
      frame.rgb = float3(0.0, 0.0, 0.0);
    }
  }
  // The gamma ramp is looked up with the value, like the unorm frame had.
  pt_output[pixel] = saturate(frame);
}
