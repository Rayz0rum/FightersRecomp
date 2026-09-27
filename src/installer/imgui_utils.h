// UI drawing helpers, ported from UnleashedRecomp's ui/imgui_utils (hedge-dev,
// GPL-3.0): shader effects through ImGui callbacks (rex/ui/imgui_effects.h),
// motion curves and text layout (with ruby annotations), drawn with the game's
// fonts.
#pragma once

#include <functional>
#include <map>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include <imgui.h>

#include <rex/ui/immediate_drawer.h>

#include "installer_common.h"
#include "installer_font.h"

#define IMGUI_SHADER_MODIFIER_NONE rex::ui::ImmediateEffect::kModifierNone
#define IMGUI_SHADER_MODIFIER_SCANLINE rex::ui::ImmediateEffect::kModifierScanline
#define IMGUI_SHADER_MODIFIER_CHECKERBOARD rex::ui::ImmediateEffect::kModifierCheckerboard
#define IMGUI_SHADER_MODIFIER_SCANLINE_BUTTON rex::ui::ImmediateEffect::kModifierScanlineButton
#define IMGUI_SHADER_MODIFIER_TEXT_SKEW rex::ui::ImmediateEffect::kModifierTextSkew
#define IMGUI_SHADER_MODIFIER_HORIZONTAL_MARQUEE_FADE \
  rex::ui::ImmediateEffect::kModifierHorizontalMarqueeFade
#define IMGUI_SHADER_MODIFIER_VERTICAL_MARQUEE_FADE \
  rex::ui::ImmediateEffect::kModifierVerticalMarqueeFade
#define IMGUI_SHADER_MODIFIER_GRAYSCALE rex::ui::ImmediateEffect::kModifierGrayscale
#define IMGUI_SHADER_MODIFIER_TITLE_BEVEL rex::ui::ImmediateEffect::kModifierTitleBevel
#define IMGUI_SHADER_MODIFIER_CATEGORY_BEVEL rex::ui::ImmediateEffect::kModifierCategoryBevel
#define IMGUI_SHADER_MODIFIER_RECTANGLE_BEVEL rex::ui::ImmediateEffect::kModifierRectangleBevel
#define IMGUI_SHADER_MODIFIER_LOW_QUALITY_TEXT rex::ui::ImmediateEffect::kModifierLowQualityText

#define PIXELS_TO_UV_COORDS(textureWidth, textureHeight, x, y, width, height)               \
  std::make_tuple(ImVec2((float)x / (float)textureWidth, (float)y / (float)textureHeight), \
                  ImVec2(((float)x + (float)width) / (float)textureWidth,                  \
                         ((float)y + (float)height) / (float)textureHeight))

#define GET_UV_COORDS(tuple) std::get<0>(tuple), std::get<1>(tuple)

#define CENTRE_TEXT_HORZ(min, max, textSize) min.x + ((max.x - min.x) - textSize.x) / 2
#define CENTRE_TEXT_VERT(min, max, textSize) min.y + ((max.y - min.y) - textSize.y) / 2

#define BREATHE_MOTION(start, end, time, rate) \
  Lerp(start, end, (sin((ImGui::GetTime() - time) * (2.0f * M_PI / rate)) + 1.0f) / 2.0f)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

constexpr float ANNOTATION_FONT_SIZE_MODIFIER = 0.6f;

struct TextSegment {
  bool annotated;
  std::string text;
  std::string annotation;
};

struct Paragraph {
  bool annotated = false;
  std::vector<std::vector<TextSegment>> lines;
};

void SetGradient(const ImVec2& min, const ImVec2& max, ImU32 top, ImU32 bottom);
void SetGradient(const ImVec2& min, const ImVec2& max, ImU32 topLeft, ImU32 topRight,
                 ImU32 bottomRight, ImU32 bottomLeft);
void ResetGradient();
void SetShaderModifier(uint32_t shaderModifier);
void SetOrigin(ImVec2 origin);
void SetScale(ImVec2 scale);
void SetTextSkew(float yCenter, float skewScale);
void ResetTextSkew();
void SetHorizontalMarqueeFade(ImVec2 min, ImVec2 max, float fadeScaleLeft, float fadeScaleRight);
void SetHorizontalMarqueeFade(ImVec2 min, ImVec2 max, float fadeScale);
void SetVerticalMarqueeFade(ImVec2 min, ImVec2 max, float fadeScaleTop, float fadeScaleBottom);
void SetVerticalMarqueeFade(ImVec2 min, ImVec2 max, float fadeScale);
void ResetMarqueeFade();
void SetOutline(float outline);
void ResetOutline();
void SetProceduralOrigin(ImVec2 proceduralOrigin);
void ResetProceduralOrigin();
void SetAdditive(bool enabled);
void ResetAdditive();
float Scale(float size);
double ComputeLinearMotion(double duration, double offset, double total);
double ComputeMotion(double duration, double offset, double total);
// Sonic the Fighters' menu panels, measured from the game at 1280x720 (sizes
// in that layout, scaled): the translucent navy body (min, max) over a white
// frame 9 units up and to the left, with the game's bevels and drop shadow.
// UnleashedRecomp: its pause container.
void DrawStfPanel(ImVec2 min, ImVec2 max, float alpha = 1.0f, bool frame = true);
// The window's corner tab with its animated dots, at the frame's top left.
void DrawStfTab(ImVec2 frameMin, float alpha = 1.0f);
// The white line under a window's title and above its choices.
void DrawStfRule(float minX, float maxX, float y, float alpha = 1.0f);
void DrawTextBasic(const InstallerFont* font, float fontSize, const ImVec2& pos, ImU32 colour,
                   const char* text);
void DrawTextWithMarquee(const InstallerFont* font, float fontSize, const ImVec2& position,
                         const ImVec2& min, const ImVec2& max, ImU32 color, const char* text,
                         double time, double delay, double speed);
void DrawTextWithOutline(const InstallerFont* font, float fontSize, const ImVec2& pos,
                         ImU32 color, const char* text, float outlineSize, ImU32 outlineColor,
                         uint32_t shaderModifier = IMGUI_SHADER_MODIFIER_NONE);
void DrawTextWithShadow(const InstallerFont* font, float fontSize, const ImVec2& pos,
                        ImU32 colour, const char* text, float offset = 2.0f, float radius = 1.0f,
                        ImU32 shadowColour = IM_COL32(0, 0, 0, 255));
float CalcWidestTextSize(const InstallerFont* font, float fontSize, std::span<std::string> strs);
std::string Truncate(const std::string& input, size_t maxLength, bool useEllipsis = true,
                     bool usePrefixEllipsis = false);
std::pair<std::string, std::map<std::string, std::string>> RemoveRubyAnnotations(
    const char* input);
std::string ReAddRubyAnnotations(const std::string_view& wrappedText,
                                 const std::map<std::string, std::string>& rubyMap);
std::vector<std::string> Split(const char* strStart, const InstallerFont* font, float fontSize,
                               float maxWidth);
Paragraph CalculateAnnotatedParagraph(const std::vector<std::string>& lines);
std::string RemoveAnnotationFromParagraphLine(const std::vector<TextSegment>& annotatedLine);
ImVec2 MeasureCentredParagraph(const InstallerFont* font, float fontSize, float lineMargin,
                               const std::vector<std::string>& lines);
ImVec2 MeasureCentredParagraph(const InstallerFont* font, float fontSize, float maxWidth,
                               float lineMargin, const char* text);
void DrawRubyAnnotatedText(const InstallerFont* font, float fontSize, float maxWidth,
                           const ImVec2& pos, float lineMargin, const char* text,
                           std::function<void(const char*, ImVec2)> drawMethod,
                           std::function<void(const char*, float, ImVec2)> annotationDrawMethod,
                           bool isCentred = false, bool leadingSpace = false);
float Lerp(float a, float b, float t);
float Cubic(float a, float b, float t);
float Hermite(float a, float b, float t);
ImVec2 Lerp(const ImVec2& a, const ImVec2& b, float t);
ImU32 ColourLerp(ImU32 c0, ImU32 c1, float t);
void DrawVersionString(const InstallerFont* font, const ImU32 col = IM_COL32(255, 255, 255, 70));
// The Sonic the Fighters menu selection bar.
void DrawSelectionContainer(ImVec2 min, ImVec2 max, float alpha = 1.0f);
void DrawToggleLight(ImVec2 pos, bool isEnabled, float alpha = 1.0f);
const char* CalcWordWrapPositionA(const InstallerFont* font, float scale, const char* text,
                                  const char* text_end, float wrap_width);
