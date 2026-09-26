// Temporal accumulation of the traced lighting: the history of the surface
// seen in each pixel is found in the previous frame with the motion from the
// primary pass (exact, from the triangle the surface was on - animated
// geometry included), validated against the previous frame's surfaces
// (disocclusion), and blended with this frame's samples. The sampling pattern
// changes every frame, so the history converges towards many samples per
// pixel.

#include "pt_common.hlsli"

// Previous frame position of the surface: output pixel xy, view depth, and
// whether it's known.
Texture2D<float4> pt_motion : register(t2);
Texture2D<float4> pt_gbuffer : register(t3);
Texture2D<float4> pt_lighting : register(t4);
Texture2D<float4> pt_previous_gbuffer : register(t5);
// Irradiance, and the number of frames accumulated in alpha.
Texture2D<float4> pt_history : register(t6);
Texture2D<float4> pt_specular_radiance : register(t0, space2);
Texture2D<float4> pt_specular_history : register(t1, space2);
RWTexture2D<float4> pt_history_out : register(u2);
RWTexture2D<float4> pt_specular_history_out : register(u3);

static const float kPTMaxHistory = 32.0;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint2 pixel = pt_rect_min + id.xy;
  if (any(pixel >= pt_rect_max)) {
    return;
  }
  float4 surface = pt_gbuffer[pixel];
  float3 irradiance = pt_lighting[pixel].rgb;
  float3 specular = pt_specular_radiance[pixel].rgb;
  float4 motion = pt_motion[pixel];
  float4 history = float4(0.0, 0.0, 0.0, 0.0);
  float3 specular_history = float3(0.0, 0.0, 0.0);
  float history_weight = 0.0;
  if (pt_history_valid && surface.w > 0.0 && motion.w > 0.0) {
    // Bilinear, with the taps showing a different surface rejected.
    float2 position = motion.xy - 0.5;
    int2 base = int2(floor(position));
    float2 fraction = position - float2(base);
    [unroll] for (uint i = 0; i < 4; ++i) {
      int2 tap = base + int2(i & 1, i >> 1);
      if (!PTInRect(tap)) {
        continue;
      }
      float4 previous_surface = pt_previous_gbuffer[tap];
      if (previous_surface.w <= 0.0 ||
          abs(previous_surface.w - motion.z) > 0.04 * motion.z + 1.0e-3 ||
          dot(previous_surface.xyz, surface.xyz) < 0.85) {
        continue;
      }
      float2 bilinear = float2((i & 1) ? fraction.x : 1.0 - fraction.x,
                               (i >> 1) ? fraction.y : 1.0 - fraction.y);
      float weight = bilinear.x * bilinear.y;
      history += pt_history[tap] * weight;
      specular_history += pt_specular_history[tap].rgb * weight;
      history_weight += weight;
    }
  }
  float frames = 1.0;
  if (history_weight > 1.0e-3) {
    history /= history_weight;
    specular_history /= history_weight;
    // Against lag (moving shadows, reflections): the history is clamped to the
    // range of this frame's samples around (mean +- 1.5 standard deviations),
    // which is wide where the samples are noisy, narrow where they agree.
    float3 mean = float3(0.0, 0.0, 0.0), mean_squared = float3(0.0, 0.0, 0.0);
    float3 specular_mean = float3(0.0, 0.0, 0.0);
    float3 specular_mean_squared = float3(0.0, 0.0, 0.0);
    float count = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y) {
      [unroll] for (int x = -1; x <= 1; ++x) {
        int2 tap = int2(pixel) + int2(x, y) * 2;
        if (!PTInRect(tap) || PTSurfaceWeight(surface, pt_gbuffer[tap]) < 0.5) {
          continue;
        }
        float3 tap_irradiance = pt_lighting[tap].rgb;
        float3 tap_specular = pt_specular_radiance[tap].rgb;
        mean += tap_irradiance;
        mean_squared += tap_irradiance * tap_irradiance;
        specular_mean += tap_specular;
        specular_mean_squared += tap_specular * tap_specular;
        count += 1.0;
      }
    }
    if (count > 0.0) {
      mean /= count;
      specular_mean /= count;
      float3 deviation = sqrt(max(mean_squared / count - mean * mean, 0.0)) * 1.5;
      float3 specular_deviation =
          sqrt(max(specular_mean_squared / count - specular_mean * specular_mean, 0.0)) * 1.5;
      history.rgb = clamp(history.rgb, mean - deviation, mean + deviation);
      specular_history =
          clamp(specular_history, specular_mean - specular_deviation,
                specular_mean + specular_deviation);
    }
    frames = min(history.a + 1.0, kPTMaxHistory);
    irradiance = lerp(history.rgb, irradiance, max(1.0 / frames, pt_temporal_alpha));
    specular = lerp(specular_history, specular, max(1.0 / frames, pt_specular_temporal_alpha));
  }
  pt_history_out[pixel] = float4(irradiance, frames);
  pt_specular_history_out[pixel] = float4(specular, 1.0);
}
