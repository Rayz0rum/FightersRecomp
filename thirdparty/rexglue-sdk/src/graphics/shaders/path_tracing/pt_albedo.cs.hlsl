// Surface colors for relighting: the scene image, except that the game's own
// shadows (black polygons on the ground) are filled in with the color of the
// ground around them, since the traced sun casts the shadows instead.

#include "pt_common.hlsli"

RWByteAddressBuffer pt_stats : register(u1);
Texture2D<float4> pt_color : register(t2);
Texture2D<float4> pt_gbuffer : register(t3);
RWTexture2D<float4> pt_albedo_out : register(u2);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  int2 pixel = int2(pt_rect_min + id.xy);
  if (!PTInRect(pixel)) {
    return;
  }
  float4 color = pt_color[pixel];
  float4 surface = pt_gbuffer[pixel];
  float shadowness = saturate((0.08 - dot(color.rgb, kPTLuminance)) * 20.0);
  if ((pt_flags & kPTFlagReplaceGameShadows) && surface.w > 0.0 && shadowness > 0.0) {
    float3 up = PTGroundUp(pt_stats.Load3(pt_stats_slot * kPTStatsSlotSize));
    if (dot(surface.xyz, up) > 0.7) {
      float3 position = PTPixelRay(float2(pixel) + 0.5) * surface.w;
      // Tight, so feet and other things standing on the ground aren't taken.
      float plane_tolerance = 0.004 * surface.w + 0.004;
      float3 sum = float3(0.0, 0.0, 0.0);
      float weight_sum = 0.0;
      // March in 16 directions along the ground to the first lit pixel, with
      // steps growing with the distance, and blend what's found by distance.
      [loop] for (uint direction_index = 0; direction_index < 16; ++direction_index) {
        float angle = (float(direction_index) + 0.5) * 0.392699082;
        float2 direction = float2(cos(angle), sin(angle));
        float distance = 0.0;
        [loop] for (uint step = 1; step <= 48; ++step) {
          distance += 1.0 + float(step) * 0.2;
          int2 tap = pixel + int2(round(direction * distance));
          if (!PTInRect(tap)) {
            break;
          }
          float4 tap_surface = pt_gbuffer[tap];
          if (tap_surface.w <= 0.0 || dot(tap_surface.xyz, surface.xyz) < 0.98) {
            break;
          }
          float3 tap_position = PTPixelRay(float2(tap) + 0.5) * tap_surface.w;
          if (abs(dot(tap_position - position, surface.xyz)) > plane_tolerance) {
            break;
          }
          if (dot(pt_color[tap].rgb, kPTLuminance) > 0.12) {
            // Past the edge of the shadow, averaged over a few texels of the
            // ground's texture, where it's still the same surface.
            int2 inner = pixel + int2(round(direction * (distance + 4.0)));
            float4 inner_surface = pt_gbuffer[inner];
            if (PTInRect(inner) && inner_surface.w > 0.0 &&
                dot(inner_surface.xyz, surface.xyz) >= 0.98 &&
                dot(pt_color[inner].rgb, kPTLuminance) > 0.12) {
              tap = inner;
            }
            float3 tap_color = float3(0.0, 0.0, 0.0);
            [unroll] for (int y = -1; y <= 1; ++y) {
              [unroll] for (int x = -1; x <= 1; ++x) {
                tap_color += pt_color[clamp(tap + int2(x, y), int2(pt_rect_min),
                                            int2(pt_rect_max) - 1)].rgb;
              }
            }
            float weight = 1.0 / (distance * distance);
            sum += tap_color * (weight / 9.0);
            weight_sum += weight;
            break;
          }
        }
      }
      if (weight_sum > 0.0) {
        color.rgb = lerp(color.rgb, sum / weight_sum, shadowness);
      }
    }
  }
  pt_albedo_out[pixel] = color;
}
