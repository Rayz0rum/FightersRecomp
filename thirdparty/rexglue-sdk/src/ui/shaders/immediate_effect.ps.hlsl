// Ported from UnleashedRecomp's ImGui pixel shader (hedge-dev, GPL-3.0):
// positions are in the coordinate space (the render target position scaled by
// PixelToCoordinates), the texture is bound directly and signed distance
// field textures are single channel (R).
#include "immediate_effect.hlsli"

float4 DecodeColor(uint color) {
  return float4(color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF, (color >> 24) & 0xFF) /
         255.0;
}

float4 SamplePoint(int2 position) {
  float4 result = 1.0;
  if (ShaderModifier == IMMEDIATE_EFFECT_MODIFIER_SCANLINE) {
    if (position.y % 2 == 0) {
      result = float4(1.0, 1.0, 1.0, 0.0);
    }
  } else if (ShaderModifier == IMMEDIATE_EFFECT_MODIFIER_CHECKERBOARD) {
    int remnantX = position.x % 9;
    int remnantY = position.y % 9;
    if (remnantX == 0 || remnantY == 0) {
      result.a = 0.0;
    }
    if ((remnantY % 2) == 0) {
      result.rgb = 0.5;
    }
  } else if (ShaderModifier == IMMEDIATE_EFFECT_MODIFIER_SCANLINE_BUTTON) {
    if (position.y % 2 == 0) {
      result = float4(1.0, 1.0, 1.0, 0.5);
    }
  }
  return result;
}

float4 SampleLinear(float2 uvTexspace) {
  int2 integerPart = int2(floor(uvTexspace));
  float2 fracPart = frac(uvTexspace);
  float4 topLeft = SamplePoint(integerPart + int2(0, 0));
  float4 topRight = SamplePoint(integerPart + int2(1, 0));
  float4 bottomLeft = SamplePoint(integerPart + int2(0, 1));
  float4 bottomRight = SamplePoint(integerPart + int2(1, 1));
  float4 top = lerp(topLeft, topRight, fracPart.x);
  float4 bottom = lerp(bottomLeft, bottomRight, fracPart.x);
  return lerp(top, bottom, fracPart.y);
}

float4 PixelAntialiasing(float2 uvTexspace) {
  if ((DisplaySize.x * InverseDisplaySize.y) >= (4.0 / 3.0)) {
    uvTexspace *= InverseDisplaySize.y * 720.0;
  } else {
    uvTexspace *= InverseDisplaySize.x * 960.0;
  }
  float2 seam = floor(uvTexspace + 0.5);
  uvTexspace = (uvTexspace - seam) / fwidth(uvTexspace) + seam;
  uvTexspace = clamp(uvTexspace, seam - 0.5, seam + 0.5);
  return SampleLinear(uvTexspace - 0.5);
}

float4 SampleSdfFont(float4 color, float2 uv, float2 screenTexSize) {
  float4 textureColor = xe_immediate_texture.Sample(xe_immediate_sampler, uv);
  uint width, height;
  xe_immediate_texture.GetDimensions(width, height);
  float pxRange = 8.0;
  float2 unitRange = pxRange / float2(width, height);
  float screenPxRange = max(0.5 * dot(unitRange, screenTexSize), 1.0);
  float sd = textureColor.r - 0.5;
  float screenPxDistance = screenPxRange * (sd + Outline / (pxRange * 1.5));
  if (ShaderModifier == IMMEDIATE_EFFECT_MODIFIER_TITLE_BEVEL) {
    float2 normal = normalize(float3(ddx(sd), ddy(sd), 0.01)).xy;
    float3 rimColor = float3(1, 0.8, 0.29);
    float3 shadowColor = float3(0.84, 0.57, 0);
    float cosTheta = dot(normal, normalize(float2(1, 1)));
    float3 gradient = lerp(color.rgb, cosTheta >= 0.0 ? rimColor : shadowColor, abs(cosTheta));
    color.rgb = lerp(gradient, color.rgb, pow(saturate(sd + 0.77), 32.0));
  } else if (ShaderModifier == IMMEDIATE_EFFECT_MODIFIER_CATEGORY_BEVEL) {
    float2 normal = normalize(float3(ddx(sd), ddy(sd), 0.25)).xy;
    float cosTheta = dot(normal, normalize(float2(1, 1)));
    float gradient = 1.0 + cosTheta * 0.5;
    color.rgb = saturate(color.rgb * gradient);
  } else if (ShaderModifier == IMMEDIATE_EFFECT_MODIFIER_TEXT_SKEW) {
    float2 normal = normalize(float3(ddx(sd), ddy(sd), 0.5)).xy;
    float cosTheta = dot(normal, normalize(float2(1, 1)));
    float gradient = saturate(1.0 + cosTheta);
    color.rgb = lerp(color.rgb * gradient, color.rgb, pow(saturate(sd + 0.77), 32.0));
  }
  color.a *= saturate(screenPxDistance + 0.5);
  return color;
}

float4 main(in Interpolators interpolators) : SV_Target {
  float2 position = interpolators.Position.xy * PixelToCoordinates;
  float4 color = interpolators.Color;
  color *= PixelAntialiasing(position - ProceduralOrigin);

  if (TextureFlags & IMMEDIATE_EFFECT_TEXTURE_SDF) {
    if (ShaderModifier == IMMEDIATE_EFFECT_MODIFIER_LOW_QUALITY_TEXT) {
      float scale;
      float invScale;
      if ((DisplaySize.x * InverseDisplaySize.y) >= (4.0 / 3.0)) {
        scale = InverseDisplaySize.y * 720.0;
        invScale = DisplaySize.y / 720.0;
      } else {
        scale = InverseDisplaySize.x * 960.0;
        invScale = DisplaySize.x / 960.0;
      }
      float2 lowQualityPosition = (position - 0.5) * scale;
      float2 fracPart = frac(lowQualityPosition);
      float2 uvStep = fwidth(interpolators.UV) * invScale;
      float2 lowQualityUV = interpolators.UV - fracPart * uvStep;
      float2 screenTexSize = 1.0 / uvStep;
      float4 topLeft = SampleSdfFont(color, lowQualityUV + float2(0, 0), screenTexSize);
      float4 topRight = SampleSdfFont(color, lowQualityUV + float2(uvStep.x, 0), screenTexSize);
      float4 bottomLeft = SampleSdfFont(color, lowQualityUV + float2(0, uvStep.y), screenTexSize);
      float4 bottomRight = SampleSdfFont(color, lowQualityUV + uvStep.xy, screenTexSize);
      float4 top = lerp(topLeft, topRight, fracPart.x);
      float4 bottom = lerp(bottomLeft, bottomRight, fracPart.x);
      color = lerp(top, bottom, fracPart.y);
    } else {
      color = SampleSdfFont(color, interpolators.UV, 1.0 / fwidth(interpolators.UV));
    }
  } else {
    color *= xe_immediate_texture.Sample(xe_immediate_sampler, interpolators.UV);
  }

  if (ShaderModifier == IMMEDIATE_EFFECT_MODIFIER_HORIZONTAL_MARQUEE_FADE) {
    float minAlpha = saturate((position.x - BoundsMin.x) / Scale.x);
    float maxAlpha = saturate((BoundsMax.x - position.x) / Scale.y);
    color.a *= minAlpha;
    color.a *= maxAlpha;
  } else if (ShaderModifier == IMMEDIATE_EFFECT_MODIFIER_VERTICAL_MARQUEE_FADE) {
    float minAlpha = saturate((position.y - BoundsMin.y) / Scale.x);
    float maxAlpha = saturate((BoundsMax.y - position.y) / Scale.y);
    color.a *= minAlpha;
    color.a *= maxAlpha;
  } else if (any(BoundsMin != BoundsMax)) {
    float2 factor = saturate((position - BoundsMin) / (BoundsMax - BoundsMin));
    if (ShaderModifier == IMMEDIATE_EFFECT_MODIFIER_RECTANGLE_BEVEL) {
      float bevelSize = 0.9;
      float shadow = saturate((factor.x - bevelSize) / (1.0 - bevelSize));
      shadow = max(shadow, saturate((factor.y - bevelSize) / (1.0 - bevelSize)));
      float rim = saturate((1.0 - factor.x - bevelSize) / (1.0 - bevelSize));
      rim = max(rim, saturate((1.0 - factor.y - bevelSize) / (1.0 - bevelSize)));
      float3 rimColor = float3(1, 0.8, 0.29);
      float3 shadowColor = float3(0.84, 0.57, 0);
      color.rgb = lerp(color.rgb, rimColor, smoothstep(0.0, 1.0, rim));
      color.rgb = lerp(color.rgb, shadowColor, smoothstep(0.0, 1.0, shadow));
    } else {
      float4 top = lerp(DecodeColor(GradientTopLeft), DecodeColor(GradientTopRight),
                        smoothstep(0.0, 1.0, factor.x));
      float4 bottom = lerp(DecodeColor(GradientBottomLeft), DecodeColor(GradientBottomRight),
                           smoothstep(0.0, 1.0, factor.x));
      color *= lerp(top, bottom, smoothstep(0.0, 1.0, factor.y));
    }
  }

  if (ShaderModifier == IMMEDIATE_EFFECT_MODIFIER_GRAYSCALE) {
    color.rgb = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
  }
  return color;
}
