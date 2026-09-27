// Ported from UnleashedRecomp's ui/imgui_utils.cpp (hedge-dev, GPL-3.0).
// Changes: text through InstallerFont, effects through rex::ui imgui_effects,
// and the window, selection and toggle light drawn in Sonic the Fighters'
// menu style instead of UnleashedRecomp's textures.

#include "imgui_utils.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <rex/ui/imgui_effects.h>

using rex::ui::AddImGuiEffectCallback;
using rex::ui::ImGuiEffectCallback;
using rex::ui::ImGuiEffectCallbackData;

extern const char* g_versionString;

namespace {

void AddCallback(ImGuiEffectCallback callback, const ImGuiEffectCallbackData& data) {
  AddImGuiEffectCallback(ImGui::GetBackgroundDrawList(), callback, data);
}

// Decodes one UTF-8 character (ImTextCharFromUtf8 semantics).
int CharFromUtf8(unsigned int* out, const char* s, const char* end) {
  auto b = uint8_t(s[0]);
  int n = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
  if (end && s + n > end) {
    *out = 0xFFFD;
    return 1;
  }
  unsigned int c = n == 1 ? b : n == 2 ? b & 0x1F : n == 3 ? b & 0x0F : b & 0x07;
  for (int i = 1; i < n; ++i) {
    c = (c << 6) | (uint8_t(s[i]) & 0x3F);
  }
  *out = c;
  return n;
}

bool CharIsBlankA(char c) {
  return c == ' ' || c == '\t';
}

bool CharIsBlankW(unsigned int c) {
  return c == ' ' || c == '\t' || c == 0x3000;
}

}  // namespace

void SetGradient(const ImVec2& min, const ImVec2& max, ImU32 top, ImU32 bottom) {
  SetGradient(min, max, top, top, bottom, bottom);
}

void SetGradient(const ImVec2& min, const ImVec2& max, ImU32 topLeft, ImU32 topRight,
                 ImU32 bottomRight, ImU32 bottomLeft) {
  ImGuiEffectCallbackData data = {};
  data.set_gradient.bounds_min[0] = min.x;
  data.set_gradient.bounds_min[1] = min.y;
  data.set_gradient.bounds_max[0] = max.x;
  data.set_gradient.bounds_max[1] = max.y;
  data.set_gradient.gradient_top_left = topLeft;
  data.set_gradient.gradient_top_right = topRight;
  data.set_gradient.gradient_bottom_right = bottomRight;
  data.set_gradient.gradient_bottom_left = bottomLeft;
  AddCallback(ImGuiEffectCallback::kSetGradient, data);
}

void ResetGradient() {
  ImGuiEffectCallbackData data = {};
  AddCallback(ImGuiEffectCallback::kSetGradient, data);
}

void SetShaderModifier(uint32_t shaderModifier) {
  ImGuiEffectCallbackData data = {};
  data.set_shader_modifier.shader_modifier = shaderModifier;
  AddCallback(ImGuiEffectCallback::kSetShaderModifier, data);
}

void SetOrigin(ImVec2 origin) {
  ImGuiEffectCallbackData data = {};
  data.set_origin.origin[0] = origin.x;
  data.set_origin.origin[1] = origin.y;
  AddCallback(ImGuiEffectCallback::kSetOrigin, data);
}

void SetScale(ImVec2 scale) {
  ImGuiEffectCallbackData data = {};
  data.set_scale.scale[0] = scale.x;
  data.set_scale.scale[1] = scale.y;
  AddCallback(ImGuiEffectCallback::kSetScale, data);
}

void SetTextSkew(float yCenter, float skewScale) {
  SetShaderModifier(IMGUI_SHADER_MODIFIER_TEXT_SKEW);
  SetOrigin({0.0f, yCenter});
  SetScale({skewScale, 1.0f});
}

void ResetTextSkew() {
  SetShaderModifier(IMGUI_SHADER_MODIFIER_NONE);
  SetOrigin({0.0f, 0.0f});
  SetScale({1.0f, 1.0f});
}

void SetHorizontalMarqueeFade(ImVec2 min, ImVec2 max, float fadeScaleLeft, float fadeScaleRight) {
  ImGuiEffectCallbackData data = {};
  data.set_marquee_fade.bounds_min[0] = min.x;
  data.set_marquee_fade.bounds_min[1] = min.y;
  data.set_marquee_fade.bounds_max[0] = max.x;
  data.set_marquee_fade.bounds_max[1] = max.y;
  AddCallback(ImGuiEffectCallback::kSetMarqueeFade, data);
  SetShaderModifier(IMGUI_SHADER_MODIFIER_HORIZONTAL_MARQUEE_FADE);
  SetScale({fadeScaleLeft, fadeScaleRight});
}

void SetHorizontalMarqueeFade(ImVec2 min, ImVec2 max, float fadeScale) {
  SetHorizontalMarqueeFade(min, max, fadeScale, fadeScale);
}

void SetVerticalMarqueeFade(ImVec2 min, ImVec2 max, float fadeScaleTop, float fadeScaleBottom) {
  ImGuiEffectCallbackData data = {};
  data.set_marquee_fade.bounds_min[0] = min.x;
  data.set_marquee_fade.bounds_min[1] = min.y;
  data.set_marquee_fade.bounds_max[0] = max.x;
  data.set_marquee_fade.bounds_max[1] = max.y;
  AddCallback(ImGuiEffectCallback::kSetMarqueeFade, data);
  SetShaderModifier(IMGUI_SHADER_MODIFIER_VERTICAL_MARQUEE_FADE);
  SetScale({fadeScaleTop, fadeScaleBottom});
}

void SetVerticalMarqueeFade(ImVec2 min, ImVec2 max, float fadeScale) {
  SetVerticalMarqueeFade(min, max, fadeScale, fadeScale);
}

void ResetMarqueeFade() {
  ResetGradient();
  SetShaderModifier(IMGUI_SHADER_MODIFIER_NONE);
  SetScale({1.0f, 1.0f});
}

void SetOutline(float outline) {
  ImGuiEffectCallbackData data = {};
  data.set_outline.outline = outline;
  AddCallback(ImGuiEffectCallback::kSetOutline, data);
}

void ResetOutline() {
  SetOutline(0.0f);
}

void SetProceduralOrigin(ImVec2 proceduralOrigin) {
  ImGuiEffectCallbackData data = {};
  data.set_procedural_origin.procedural_origin[0] = proceduralOrigin.x;
  data.set_procedural_origin.procedural_origin[1] = proceduralOrigin.y;
  AddCallback(ImGuiEffectCallback::kSetProceduralOrigin, data);
}

void ResetProceduralOrigin() {
  SetProceduralOrigin({0.0f, 0.0f});
}

void SetAdditive(bool enabled) {
  ImGuiEffectCallbackData data = {};
  data.set_additive.enabled = enabled;
  AddCallback(ImGuiEffectCallback::kSetAdditive, data);
}

void ResetAdditive() {
  SetAdditive(false);
}

float Scale(float size) {
  return size * g_aspectRatioScale;
}

double ComputeLinearMotion(double duration, double offset, double total) {
  return std::clamp((ImGui::GetTime() - duration - offset / 60.0) / total * 60.0, 0.0, 1.0);
}

double ComputeMotion(double duration, double offset, double total) {
  return sqrt(ComputeLinearMotion(duration, offset, total));
}

namespace {

ImU32 StfColour(int r, int g, int b, float a, float alpha) {
  return IM_COL32(r, g, b, int(std::clamp(a * alpha, 0.0f, 255.0f)));
}

// A band of the frame along one side of (min, max): dark, white, then grey
// going inwards.
void DrawStfFrameBand(ImDrawList* drawList, ImVec2 min, ImVec2 max, int side, float alpha) {
  struct Layer {
    float from, to;
    int grey;
  };
  constexpr Layer kLayers[] = {{0, 2, 81}, {2, 8, 255}, {8, 9, 205}};
  for (const Layer& layer : kLayers) {
    ImVec2 a = min, b = max;
    switch (side) {
      case 0: a.y = min.y + Scale(layer.from); b.y = min.y + Scale(layer.to); break;  // top
      case 1: a.x = min.x + Scale(layer.from); b.x = min.x + Scale(layer.to); break;  // left
      case 2: a.x = max.x - Scale(layer.to); b.x = max.x - Scale(layer.from); break;  // right
      default: a.y = max.y - Scale(layer.to); b.y = max.y - Scale(layer.from); break;  // bottom
    }
    drawList->AddRectFilled(a, b, StfColour(layer.grey, layer.grey, layer.grey, 255, alpha));
  }
}

}  // namespace

void DrawStfPanel(ImVec2 min, ImVec2 max, float alpha, bool frame) {
  // Measured from the game's menus at 1280x720.
  auto drawList = ImGui::GetBackgroundDrawList();
  auto u = Scale(1.0f);
  auto c = [&](int r, int g, int b, float a) { return StfColour(r, g, b, a, alpha); };
  auto hGradient = [&](float x0, float x1, float y0, float y1, ImU32 left, ImU32 right) {
    drawList->AddRectFilledMultiColor({x0, y0}, {x1, y1}, left, right, right, left);
  };
  auto vGradient = [&](float x0, float x1, float y0, float y1, ImU32 top, ImU32 bottom) {
    drawList->AddRectFilledMultiColor({x0, y0}, {x1, y1}, top, top, bottom, bottom);
  };

  // Drop shadow, below and to the right.
  vGradient(min.x + 8 * u, max.x + 8 * u, max.y, max.y + 12 * u, c(0, 0, 0, 205), c(0, 0, 0, 0));
  hGradient(max.x, max.x + 9 * u, min.y + 8 * u, max.y, c(0, 0, 0, 190), c(0, 0, 0, 0));

  // The white frame behind: only its top and left show outside the body.
  if (frame) {
    ImVec2 fmin = {min.x - 9 * u, min.y - 9 * u}, fmax = {max.x - 9 * u, max.y - 9 * u};
    DrawStfFrameBand(drawList, fmin, {fmax.x, min.y}, 0, alpha);
    DrawStfFrameBand(drawList, {fmin.x, fmin.y + 2 * u}, {min.x, fmax.y}, 1, alpha);
    DrawStfFrameBand(drawList, {min.x, fmin.y}, {fmax.x, min.y}, 2, alpha);
    DrawStfFrameBand(drawList, {fmin.x, min.y}, {min.x, fmax.y}, 3, alpha);
  }

  // Body.
  drawList->AddRectFilled(min, max, c(2, 18, 50, 207));
  // Light edge at the top and left, glowing inwards.
  drawList->AddRectFilled({min.x, min.y + u}, {max.x, min.y + 2 * u}, c(112, 116, 124, 255));
  drawList->AddRectFilled({min.x, min.y + 2 * u}, {max.x, min.y + 3 * u}, c(166, 177, 202, 235));
  vGradient(min.x, max.x, min.y + 3 * u, min.y + 13 * u, c(140, 155, 185, 110), c(140, 155, 185, 0));
  drawList->AddRectFilled({min.x + u, min.y}, {min.x + 2 * u, max.y}, c(95, 100, 112, 200));
  hGradient(min.x + 2 * u, min.x + 12 * u, min.y, max.y, c(130, 145, 175, 95), c(130, 145, 175, 0));
  // Rails inside the right and bottom edges, then a glow up to the edge.
  for (int i = 0; i < 3; ++i) {
    float d = 14.0f - 2.0f * i;
    ImU32 line = i == 1 ? c(150, 165, 200, 50) : c(0, 6, 22, 120);
    drawList->AddRectFilled({max.x - d * u, min.y}, {max.x - (d - 1.5f) * u, max.y}, line);
    drawList->AddRectFilled({min.x, max.y - d * u}, {max.x, max.y - (d - 1.5f) * u}, line);
  }
  hGradient(max.x - 8 * u, max.x - 2 * u, min.y, max.y, c(140, 155, 185, 0), c(140, 155, 185, 120));
  vGradient(min.x, max.x, max.y - 8 * u, max.y - 2 * u, c(140, 155, 185, 0), c(140, 155, 185, 120));
  drawList->AddRectFilled({max.x - 2 * u, min.y}, {max.x - u, max.y}, c(150, 160, 185, 170));
  drawList->AddRectFilled({min.x, max.y - 2 * u}, {max.x, max.y - u}, c(150, 160, 185, 190));
  // Outline.
  drawList->AddRectFilled(min, {max.x, min.y + u}, c(76, 76, 76, 255));
  drawList->AddRectFilled(min, {min.x + u, max.y}, c(76, 76, 76, 255));
  drawList->AddRectFilled({max.x - u, min.y}, max, c(33, 33, 33, 255));
  drawList->AddRectFilled({min.x, max.y - u}, max, c(33, 33, 33, 255));
}

void DrawStfTab(ImVec2 frameMin, float alpha) {
  auto drawList = ImGui::GetBackgroundDrawList();
  // The game's sprite, drawn at its 1920x1080 size.
  constexpr float kScale = 2.0f / 3.0f;
  if (GuestTexture* tab = InstallerAssets::Texture("window_tab")) {
    drawList->AddImage(TexRef(tab), frameMin,
                       {frameMin.x + Scale(tab->width * kScale), frameMin.y + Scale(tab->height * kScale)},
                       {0, 0}, {1, 1}, IM_COL32(255, 255, 255, int(255 * alpha)));
  }
  // The dots: a gap goes round the outer eight clockwise from the top right,
  // once a second, and each dot fades back in behind it.
  constexpr int kRing[8][2] = {{2, 0}, {2, 1}, {2, 2}, {1, 2}, {0, 2}, {0, 1}, {0, 0}, {1, 0}};
  double time = ImGui::GetTime();
  for (int j = 0; j < 3; ++j) {
    for (int i = 0; i < 3; ++i) {
      float brightness = 1.0f;
      for (int k = 0; k < 8; ++k) {
        if (kRing[k][0] == i && kRing[k][1] == j) {
          double since = std::fmod(time - k / 8.0 + 8.0, 1.0);
          brightness = float(std::clamp((since - 0.06) / 0.4, 0.0, 1.0));
        }
      }
      ImVec2 p = {frameMin.x + Scale((14 + 10 * i) * kScale), frameMin.y + Scale((11 + 10 * j) * kScale)};
      drawList->AddRectFilled(p, {p.x + Scale(6 * kScale), p.y + Scale(6 * kScale)},
                              IM_COL32(141, 141, 141, int(255 * alpha * brightness)));
    }
  }
}

void DrawStfRule(float minX, float maxX, float y, float alpha) {
  auto drawList = ImGui::GetBackgroundDrawList();
  drawList->AddRectFilled({minX, y}, {maxX, y + Scale(2)}, StfColour(252, 253, 253, 255, alpha));
  drawList->AddRectFilled({minX, y + Scale(2)}, {maxX, y + Scale(3)}, StfColour(110, 116, 132, 200, alpha));
}

void DrawTextBasic(const InstallerFont* font, float fontSize, const ImVec2& pos, ImU32 colour,
                   const char* text) {
  font->AddText(ImGui::GetBackgroundDrawList(), fontSize, pos, colour, text, nullptr);
}

void DrawTextWithMarquee(const InstallerFont* font, float fontSize, const ImVec2& position,
                         const ImVec2& min, const ImVec2& max, ImU32 color, const char* text,
                         double time, double delay, double speed) {
  auto drawList = ImGui::GetBackgroundDrawList();
  auto rectWidth = max.x - min.x;
  auto textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0, text);
  auto textX = position.x - fmodf(float(std::max(0.0, ImGui::GetTime() - (time + delay)) * speed),
                                  textSize.x + rectWidth);

  drawList->PushClipRect(min, max, true);

  if (textX <= position.x) {
    DrawRubyAnnotatedText(
        font, fontSize, FLT_MAX, {textX, position.y}, 0.0f, text,
        [=](const char* str, ImVec2 pos) { DrawTextBasic(font, fontSize, pos, color, str); },
        [=](const char* str, float size, ImVec2 pos) {
          DrawTextBasic(font, size, pos, color, str);
        });
  }

  if (textX + textSize.x < position.x) {
    DrawRubyAnnotatedText(
        font, fontSize, FLT_MAX, {textX + textSize.x + rectWidth, position.y}, 0.0f, text,
        [=](const char* str, ImVec2 pos) { DrawTextBasic(font, fontSize, pos, color, str); },
        [=](const char* str, float size, ImVec2 pos) {
          DrawTextBasic(font, size, pos, color, str);
        });
  }

  drawList->PopClipRect();
}

void DrawTextWithOutline(const InstallerFont* font, float fontSize, const ImVec2& pos,
                         ImU32 color, const char* text, float outlineSize, ImU32 outlineColor,
                         uint32_t shaderModifier) {
  auto drawList = ImGui::GetBackgroundDrawList();

  SetOutline(outlineSize);
  font->AddText(drawList, fontSize, pos, outlineColor, text);
  ResetOutline();

  if (shaderModifier != IMGUI_SHADER_MODIFIER_NONE) SetShaderModifier(shaderModifier);

  font->AddText(drawList, fontSize, pos, color, text);

  if (shaderModifier != IMGUI_SHADER_MODIFIER_NONE) SetShaderModifier(IMGUI_SHADER_MODIFIER_NONE);
}

void DrawTextWithShadow(const InstallerFont* font, float fontSize, const ImVec2& pos,
                        ImU32 colour, const char* text, float offset, float radius,
                        ImU32 shadowColour) {
  auto drawList = ImGui::GetBackgroundDrawList();

  offset = Scale(offset);

  SetOutline(radius);
  font->AddText(drawList, fontSize, {pos.x + offset, pos.y + offset}, shadowColour, text);
  ResetOutline();

  font->AddText(drawList, fontSize, pos, colour, text, nullptr);
}

float CalcWidestTextSize(const InstallerFont* font, float fontSize, std::span<std::string> strs) {
  auto result = 0.0f;

  for (auto& str : strs)
    result = std::max(result, font->CalcTextSizeA(fontSize, FLT_MAX, 0, str.c_str()).x);

  return result;
}

std::string Truncate(const std::string& input, size_t maxLength, bool useEllipsis,
                     bool usePrefixEllipsis) {
  const std::string ellipsis = "...";

  if (input.length() > maxLength) {
    if (useEllipsis && maxLength > ellipsis.length()) {
      if (usePrefixEllipsis) {
        return ellipsis + input.substr(0, maxLength - ellipsis.length());
      } else {
        return input.substr(0, maxLength - ellipsis.length()) + ellipsis;
      }
    } else {
      return input.substr(0, maxLength);
    }
  }

  return input;
}

std::pair<std::string, std::map<std::string, std::string>> RemoveRubyAnnotations(
    const char* input) {
  std::string output;
  std::map<std::string, std::string> rubyMap;
  std::string currentMain, currentRuby;
  size_t idx = 0;

  while (input[idx] != '\0') {
    if (input[idx] == '[') {
      idx++;
      currentMain.clear();
      currentRuby.clear();

      while (input[idx] != ':' && input[idx] != ']' && input[idx] != '\0') {
        currentMain += input[idx++];
      }
      if (input[idx] == ':') {
        idx++;
        while (input[idx] != ']' && input[idx] != '\0') {
          currentRuby += input[idx++];
        }
      }
      if (input[idx] == ']') {
        idx++;
      }

      if (!currentMain.empty() && !currentRuby.empty()) {
        rubyMap[currentMain] = currentRuby;
      }

      output += currentMain;
    } else {
      output += input[idx++];
    }
  }

  return {output, rubyMap};
}

std::string ReAddRubyAnnotations(const std::string_view& wrappedText,
                                 const std::map<std::string, std::string>& rubyMap) {
  std::string annotatedText;
  size_t idx = 0;
  size_t length = wrappedText.length();

  while (idx < length) {
    bool matched = false;
    for (const auto& [mainText, rubyText] : rubyMap) {
      if (wrappedText.substr(idx, mainText.length()) == mainText) {
        annotatedText += "[";
        annotatedText += mainText;
        annotatedText += ":";
        annotatedText += rubyText;
        annotatedText += "]";

        idx += mainText.length();
        matched = true;
        break;
      }
    }
    if (!matched) {
      annotatedText += wrappedText[idx++];
    }
  }

  return annotatedText;
}

std::vector<std::string> Split(const char* strStart, const InstallerFont* font, float fontSize,
                               float maxWidth) {
  if (!strStart) return {};

  std::vector<std::string> result;
  float textWidth = 0.0f;
  float lineWidth = 0.0f;
  const float scale = fontSize / font->FontSize;
  const char* str = strStart;
  const char* strEnd = strStart + strlen(strStart);
  const char* lineStart = strStart;
  const bool wordWrapEnabled = (maxWidth > 0.0f);
  const char* wordWrapEOL = nullptr;

  auto IsKanji = [](const char* str, const char* strEnd) {
    unsigned int c = (unsigned char)*str;
    if (c >= 0x80) CharFromUtf8(&c, str, strEnd);

    // Basic CJK and CJK Extension A
    return (c >= 0x4E00 && c <= 0x9FBF) || (c >= 0x3400 && c <= 0x4DBF);
  };

  while (*str != 0) {
    if (wordWrapEnabled) {
      if (wordWrapEOL == nullptr) {
        wordWrapEOL = CalcWordWrapPositionA(font, scale, str, strEnd, maxWidth - lineWidth);
      }

      if (str >= wordWrapEOL) {
        if (IsKanji(str, strEnd)) {
          // If the current character is Kanji, move back to prevent splitting Kanji
          while (str > lineStart && IsKanji(str - 3, strEnd)) {
            str -= 3;
          }
        }

        if (textWidth < lineWidth) textWidth = lineWidth;

        result.emplace_back(lineStart, str);
        lineWidth = 0.0f;
        wordWrapEOL = nullptr;

        while (str < strEnd && CharIsBlankA(*str)) str++;

        if (*str == '\n') str++;

        if (strncmp(str, "​", 3) == 0) {
          str += 3;
        }

        lineStart = str;
        continue;
      }
    }

    const char* prevStr = str;
    unsigned int c = (unsigned char)*str;
    if (c < 0x80)
      str += 1;
    else
      str += CharFromUtf8(&c, str, strEnd);

    if (c < 32) {
      if (c == '\n') {
        result.emplace_back(lineStart, str - 1);
        lineStart = str;
        textWidth = std::max(textWidth, lineWidth);
        lineWidth = 0.0f;
        continue;
      }

      if (c == '\r') {
        lineStart = str;
        continue;
      }
    }

    const float charWidth = font->GetCharAdvance(c) * scale;
    if (lineWidth + charWidth >= maxWidth) {
      str = prevStr;
      break;
    }

    lineWidth += charWidth;
  }

  if (str != lineStart) {
    result.emplace_back(lineStart, str);
  }

  return result;
}

Paragraph CalculateAnnotatedParagraph(const std::vector<std::string>& lines) {
  Paragraph result;

  for (const auto& line : lines) {
    std::vector<TextSegment> segments;

    size_t pos = 0;
    size_t start = 0;

    while ((start = line.find('[', pos)) != std::string::npos) {
      size_t end = line.find(']', start);
      if (end == std::string::npos) break;

      size_t colon = line.find(':', start);
      if (colon != std::string::npos && colon < end) {
        if (start != pos) {
          segments.push_back({false, line.substr(pos, start - pos), ""});
        }

        segments.push_back({true, line.substr(start + 1, colon - start - 1),
                            line.substr(colon + 1, end - colon - 1)});

        result.annotated = true;
        pos = end + 1;
      } else {
        pos = start + 1;
      }
    }

    if (pos < line.size()) {
      segments.push_back({false, line.substr(pos), ""});
    }

    result.lines.push_back(segments);
  }

  return result;
}

std::string RemoveAnnotationFromParagraphLine(const std::vector<TextSegment>& annotatedLine) {
  std::string result = "";

  for (auto& segment : annotatedLine) result += segment.text;

  return result;
}

ImVec2 MeasureCentredParagraph(const InstallerFont* font, float fontSize, float lineMargin,
                               const std::vector<std::string>& lines) {
  auto x = 0.0f;
  auto y = 0.0f;

  const auto paragraph = CalculateAnnotatedParagraph(lines);

  std::vector<std::string> annotationRemovedLines;

  for (const auto& line : paragraph.lines)
    annotationRemovedLines.emplace_back(RemoveAnnotationFromParagraphLine(line));

  for (size_t i = 0; i < annotationRemovedLines.size(); i++) {
    auto textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0, annotationRemovedLines[i].c_str());

    x = std::max(x, textSize.x);
    y += textSize.y + Scale(lineMargin);

    if (paragraph.annotated && i != (annotationRemovedLines.size() - 1))
      y += fontSize * ANNOTATION_FONT_SIZE_MODIFIER;
  }

  return {x, y};
}

ImVec2 MeasureCentredParagraph(const InstallerFont* font, float fontSize, float maxWidth,
                               float lineMargin, const char* text) {
  const auto input = RemoveRubyAnnotations(text);
  auto lines = Split(input.first.c_str(), font, fontSize, maxWidth);

  for (auto& line : lines) line = ReAddRubyAnnotations(line, input.second);

  return MeasureCentredParagraph(font, fontSize, lineMargin, lines);
}

void DrawRubyAnnotatedText(const InstallerFont* font, float fontSize, float maxWidth,
                           const ImVec2& pos, float lineMargin, const char* text,
                           std::function<void(const char*, ImVec2)> drawMethod,
                           std::function<void(const char*, float, ImVec2)> annotationDrawMethod,
                           bool isCentred, bool leadingSpace) {
  auto annotationFontSize = fontSize * ANNOTATION_FONT_SIZE_MODIFIER;

  const auto input = RemoveRubyAnnotations(text);
  auto lines = Split(input.first.c_str(), font, fontSize, maxWidth);

  for (auto& line : lines) {
    line = ReAddRubyAnnotations(line, input.second);
    if (!line.empty() && line.substr(0, 3) != "「" && leadingSpace) {
      line.insert(0, " ");
    }
  }

  auto paragraphSize = MeasureCentredParagraph(font, fontSize, lineMargin, lines);
  auto offsetY = 0.0f;

  const auto paragraph = CalculateAnnotatedParagraph(lines);

  for (const auto& annotatedLine : paragraph.lines) {
    const auto annotationRemovedLine = RemoveAnnotationFromParagraphLine(annotatedLine);

    auto textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0, annotationRemovedLine.c_str());
    auto annotationSize = font->CalcTextSizeA(annotationFontSize, FLT_MAX, 0, "");
    auto textX = pos.x;
    auto textY = pos.y + offsetY;

    if (isCentred) {
      textX = annotationRemovedLine.starts_with("- ") ? pos.x - paragraphSize.x / 2
                                                      : pos.x - textSize.x / 2;

      textY = pos.y - paragraphSize.y / 2 + offsetY;
    }

    for (const auto& segment : annotatedLine) {
      textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0, segment.text.c_str());

      if (segment.annotated) {
        annotationSize = font->CalcTextSizeA(annotationFontSize, FLT_MAX, 0, segment.annotation.c_str());
        float annotationX = textX + (textSize.x - annotationSize.x) / 2.0f;

        annotationDrawMethod(segment.annotation.c_str(), annotationFontSize,
                             {annotationX, textY - annotationFontSize});
      }

      drawMethod(segment.text.c_str(), {textX, textY});

      textX += textSize.x;
    }

    offsetY += textSize.y + Scale(lineMargin);

    if (paragraph.annotated) offsetY += annotationSize.y;
  }
}

float Lerp(float a, float b, float t) {
  return a + (b - a) * t;
}

float Cubic(float a, float b, float t) {
  return a + (b - a) * (t * t * t);
}

float Hermite(float a, float b, float t) {
  return a + (b - a) * (t * t * (3 - 2 * t));
}

ImVec2 Lerp(const ImVec2& a, const ImVec2& b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

ImU32 ColourLerp(ImU32 c0, ImU32 c1, float t) {
  auto a = ImGui::ColorConvertU32ToFloat4(c0);
  auto b = ImGui::ColorConvertU32ToFloat4(c1);

  ImVec4 result;
  result.x = a.x + (b.x - a.x) * t;
  result.y = a.y + (b.y - a.y) * t;
  result.z = a.z + (b.z - a.z) * t;
  result.w = a.w + (b.w - a.w) * t;

  return ImGui::ColorConvertFloat4ToU32(result);
}

void DrawVersionString(const InstallerFont* font, const ImU32 col) {
  auto drawList = ImGui::GetBackgroundDrawList();
  auto& res = ImGui::GetIO().DisplaySize;
  auto fontSize = Scale(12);
  auto textMargin = Scale(2);
  auto textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0, g_versionString);

  font->AddText(drawList, fontSize, {res.x - textSize.x - textMargin, res.y - textSize.y - textMargin},
                col, g_versionString);
}

void DrawSelectionContainer(ImVec2 min, ImVec2 max, float alpha) {
  // The game's selection bar: flat blue, darkening over its last 149 units at
  // each end, with light edges.
  auto drawList = ImGui::GetBackgroundDrawList();
  auto c = [&](int r, int g, int b, float a) { return StfColour(r, g, b, a, alpha); };
  float fade = std::min(Scale(149), (max.x - min.x) / 2);
  drawList->AddRectFilled(min, max, c(2, 125, 198, 245));
  drawList->AddRectFilledMultiColor(min, {min.x + fade, max.y}, c(11, 21, 44, 190), c(11, 21, 44, 0),
                                    c(11, 21, 44, 0), c(11, 21, 44, 190));
  drawList->AddRectFilledMultiColor({max.x - fade, min.y}, max, c(11, 21, 44, 0), c(11, 21, 44, 190),
                                    c(11, 21, 44, 190), c(11, 21, 44, 0));
  drawList->AddRectFilled(min, {max.x, min.y + Scale(1.5f)}, c(20, 150, 225, 200));
  drawList->AddRectFilled({min.x, max.y - Scale(1.5f)}, max, c(10, 140, 215, 255));
}

void DrawToggleLight(ImVec2 pos, bool isEnabled, float alpha) {
  // A small lamp: lit cyan with a glow when enabled.
  auto drawList = ImGui::GetBackgroundDrawList();
  auto lightSize = Scale(14);
  ImVec2 centre = {pos.x + lightSize * 0.5f, pos.y + lightSize * 0.5f};
  float radius = lightSize * 0.5f;

  if (isEnabled) {
    if (GuestTexture* glow = InstallerAssets::Texture("ring_glow")) {
      float glowSize = Scale(30);
      SetAdditive(true);
      drawList->AddImage(TexRef(glow), {centre.x - glowSize * 0.5f, centre.y - glowSize * 0.5f},
                         {centre.x + glowSize * 0.5f, centre.y + glowSize * 0.5f}, {0, 0}, {1, 1},
                         IM_COL32(0, 200, 255, int(160 * alpha)));
      SetAdditive(false);
    }
    drawList->AddCircleFilled(centre, radius, IM_COL32(40, 220, 255, int(255 * alpha)), 20);
    drawList->AddCircleFilled({centre.x - radius * 0.25f, centre.y - radius * 0.3f},
                              radius * 0.35f, IM_COL32(220, 250, 255, int(230 * alpha)), 12);
  } else {
    drawList->AddCircleFilled(centre, radius, IM_COL32(20, 30, 60, int(255 * alpha)), 20);
  }
  drawList->AddCircle(centre, radius, IM_COL32(200, 220, 255, int(200 * alpha)), 20,
                      Scale(1.5f));
}

// Taken from ImGui because we need to modify to break for '​\ too
// Simple word-wrapping for English, not full-featured. Please submit failing cases!
// This will return the next location to wrap from. If no wrapping if necessary, this will
// fast-forward to e.g. text_end.
const char* CalcWordWrapPositionA(const InstallerFont* font, float scale, const char* text,
                                  const char* text_end, float wrap_width) {
  float line_width = 0.0f;
  float word_width = 0.0f;
  float blank_width = 0.0f;
  wrap_width /= scale;  // We work with unscaled widths to avoid scaling every characters

  const char* word_end = text;
  const char* prev_word_end = NULL;
  bool inside_word = true;

  const char* s = text;
  IM_ASSERT(text_end != NULL);
  while (s < text_end) {
    unsigned int c = (unsigned char)*s;
    const char* next_s;
    if (c < 0x80)
      next_s = s + 1;
    else
      next_s = s + CharFromUtf8(&c, s, text_end);

    if (c < 32) {
      if (c == '\n') {
        line_width = word_width = blank_width = 0.0f;
        inside_word = true;
        s = next_s;
        continue;
      }
      if (c == '\r') {
        s = next_s;
        continue;
      }
    }

    const float char_width = font->GetCharAdvance(c);
    if (CharIsBlankW(c) || c == 0x200B) {
      if (inside_word) {
        line_width += blank_width;
        blank_width = 0.0f;
        word_end = s;
      }
      blank_width += char_width;
      inside_word = false;
    } else {
      word_width += char_width;
      if (inside_word) {
        word_end = next_s;
      } else {
        prev_word_end = word_end;
        line_width += word_width + blank_width;
        word_width = blank_width = 0.0f;
      }

      // Allow wrapping after punctuation.
      inside_word = (c != '.' && c != ',' && c != ';' && c != '!' && c != '?' && c != '\"');
    }

    // We ignore blank width at the end of the line (they can be skipped)
    if (line_width + word_width > wrap_width) {
      // Words that cannot possibly fit within an entire line will be cut anywhere.
      if (word_width < wrap_width) s = prev_word_end ? prev_word_end : word_end;
      break;
    }

    s = next_s;
  }

  // Wrap_width is too small to fit anything. Force displaying 1 character to minimize the height
  // discontinuity. +1 may not be a character start point in UTF-8 but it's ok because caller
  // loops use (text >= word_wrap_eol).
  if (s == text && text < text_end) return s + 1;
  return s;
}
