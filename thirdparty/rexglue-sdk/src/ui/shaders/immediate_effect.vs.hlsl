// Ported from UnleashedRecomp's ImGui vertex shader (hedge-dev, GPL-3.0).
#include "immediate_effect.hlsli"

void main(in float2 position : POSITION, in float2 uv : TEXCOORD, in float4 color : COLOR,
          out Interpolators interpolators) {
  if (ShaderModifier == IMMEDIATE_EFFECT_MODIFIER_TEXT_SKEW) {
    if (position.y < Origin.y) {
      position.x += Scale.x;
    }
  } else if (ShaderModifier != IMMEDIATE_EFFECT_MODIFIER_HORIZONTAL_MARQUEE_FADE &&
             ShaderModifier != IMMEDIATE_EFFECT_MODIFIER_VERTICAL_MARQUEE_FADE) {
    position.xy = Origin + (position.xy - Origin) * Scale;
  }
  interpolators.Position =
      float4(position.xy * InverseDisplaySize * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  interpolators.UV = uv;
  interpolators.Color = color;
}
