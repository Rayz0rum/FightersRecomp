// ImGui effect shaders for the immediate drawer - gradients, procedural
// patterns, signed distance field text with outlines and bevels, transforms
// and marquee fades. Ported from UnleashedRecomp's ImGui shaders (hedge-dev,
// GPL-3.0), adapted to the immediate drawer's bindings.

#ifndef REX_UI_IMMEDIATE_EFFECT_HLSLI_
#define REX_UI_IMMEDIATE_EFFECT_HLSLI_

#define IMMEDIATE_EFFECT_MODIFIER_NONE                     0
#define IMMEDIATE_EFFECT_MODIFIER_SCANLINE                 1
#define IMMEDIATE_EFFECT_MODIFIER_CHECKERBOARD             2
#define IMMEDIATE_EFFECT_MODIFIER_SCANLINE_BUTTON          3
#define IMMEDIATE_EFFECT_MODIFIER_TEXT_SKEW                4
#define IMMEDIATE_EFFECT_MODIFIER_HORIZONTAL_MARQUEE_FADE  5
#define IMMEDIATE_EFFECT_MODIFIER_VERTICAL_MARQUEE_FADE    6
#define IMMEDIATE_EFFECT_MODIFIER_GRAYSCALE                7
#define IMMEDIATE_EFFECT_MODIFIER_TITLE_BEVEL              8
#define IMMEDIATE_EFFECT_MODIFIER_CATEGORY_BEVEL           9
#define IMMEDIATE_EFFECT_MODIFIER_RECTANGLE_BEVEL          10
#define IMMEDIATE_EFFECT_MODIFIER_LOW_QUALITY_TEXT         11

// ImmediateEffect::texture_flags.
#define IMMEDIATE_EFFECT_TEXTURE_SDF 1u

// Must match rex::ui::ImmediateEffect.
cbuffer EffectConstants : register(b1) {
  float2 BoundsMin;
  float2 BoundsMax;
  uint GradientTopLeft;
  uint GradientTopRight;
  uint GradientBottomRight;
  uint GradientBottomLeft;
  uint ShaderModifier;
  uint TextureFlags;
  // The coordinate space (ImGui display size).
  float2 DisplaySize;
  float2 InverseDisplaySize;
  float2 Origin;
  float2 Scale;
  float2 ProceduralOrigin;
  float Outline;
  float Padding0;
  // Render target pixels to coordinate space units.
  float2 PixelToCoordinates;
};

Texture2D<float4> xe_immediate_texture : register(t0);
SamplerState xe_immediate_sampler : register(s0);

struct Interpolators {
  float4 Position : SV_Position;
  float2 UV : TEXCOORD;
  float4 Color : COLOR;
};

#endif  // REX_UI_IMMEDIATE_EFFECT_HLSLI_
