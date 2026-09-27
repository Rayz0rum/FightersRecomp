// Ported from UnleashedRecomp's ui/button_guide.cpp (hedge-dev, GPL-3.0).

#include "button_guide.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "imgui_utils.h"
#include "installer_pack.h"

constexpr float DEFAULT_SIDE_MARGINS = 379;

float g_sideMargins = DEFAULT_SIDE_MARGINS;

static std::vector<Button> g_buttons;

std::unordered_map<EButtonIcon, float> g_iconWidths = {
    {EButtonIcon::A, 40},     {EButtonIcon::B, 40},     {EButtonIcon::X, 40},
    {EButtonIcon::Y, 40},     {EButtonIcon::LB, 70},    {EButtonIcon::RB, 70},
    {EButtonIcon::LT, 42},    {EButtonIcon::RT, 42},    {EButtonIcon::Start, 60},
    {EButtonIcon::Back, 60},  {EButtonIcon::LMB, 40},   {EButtonIcon::Enter, 40},
    {EButtonIcon::Escape, 40},
};

std::unordered_map<EButtonIcon, float> g_iconHeights = {
    {EButtonIcon::A, 40},     {EButtonIcon::B, 40},     {EButtonIcon::X, 40},
    {EButtonIcon::Y, 40},     {EButtonIcon::LB, 40},    {EButtonIcon::RB, 40},
    {EButtonIcon::LT, 42},    {EButtonIcon::RT, 42},    {EButtonIcon::Start, 40},
    {EButtonIcon::Back, 40},  {EButtonIcon::LMB, 40},   {EButtonIcon::Enter, 40},
    {EButtonIcon::Escape, 40},
};

// The icon's texture and UVs.
static bool GetButtonIcon(EButtonIcon icon, GuestTexture*& texture, ImVec2& uvMin, ImVec2& uvMax) {
  if (icon == EButtonIcon::LMB || icon == EButtonIcon::Enter || icon == EButtonIcon::Escape) {
    texture = InstallerAssets::Texture("kbm");
    int column = icon == EButtonIcon::LMB ? 0 : icon == EButtonIcon::Enter ? 1 : 2;
    auto uv = PIXELS_TO_UV_COORDS(384, 128, column * 128, 0, 128, 128);
    uvMin = std::get<0>(uv);
    uvMax = std::get<1>(uv);
    return texture != nullptr;
  }

  bool playStation = hid::g_inputDevice == hid::EInputDevice::PlayStation;
  uint32_t id;
  switch (icon) {
    case EButtonIcon::A: id = playStation ? kButtonPSCross : kButtonXboxA; break;
    case EButtonIcon::B: id = playStation ? kButtonPSCircle : kButtonXboxB; break;
    case EButtonIcon::X: id = playStation ? kButtonPSSquare : kButtonXboxX; break;
    case EButtonIcon::Y: id = playStation ? kButtonPSTriangle : kButtonXboxY; break;
    case EButtonIcon::LB: id = kButtonXboxLB; break;
    case EButtonIcon::RB: id = kButtonXboxRB; break;
    case EButtonIcon::LT: id = kButtonXboxLT; break;
    case EButtonIcon::RT: id = kButtonXboxRT; break;
    case EButtonIcon::Start: id = kButtonXboxStart; break;
    default: id = kButtonXboxBack; break;
  }
  ImVec2 size;
  return InstallerAssets::Button(id, texture, uvMin, uvMax, size);
}

static void DrawGuide(float* offset, ImVec2 regionMin, ImVec2 regionMax, EButtonIcon icon,
                      EButtonAlignment alignment, ImVec2 iconMin, ImVec2 iconMax,
                      float textWidth, float maxTextWidth, float textScale, float fontSize,
                      const char* text) {
  (void)textWidth;
  (void)maxTextWidth;
  auto drawList = ImGui::GetBackgroundDrawList();
  auto textMarginY = regionMin.y + Scale(9.0f);

  GuestTexture* texture;
  ImVec2 uvMin, uvMax;
  if (GetButtonIcon(icon, texture, uvMin, uvMax)) {
    drawList->AddImage(TexRef(texture), iconMin, iconMax, uvMin, uvMax);
  }

  auto textMarginX = alignment == EButtonAlignment::Left ? regionMin.x + *offset
                                                          : regionMax.x - *offset;

  ImVec2 textPos = {textMarginX, textMarginY};

  SetScale({textScale, 1.0f});
  SetOrigin(textPos);

  DrawTextWithOutline(InstallerAssets::BodyFont(), fontSize, textPos, IM_COL32_WHITE, text, 4,
                      IM_COL32_BLACK);

  SetScale({1.0f, 1.0f});
  SetOrigin({0.0f, 0.0f});
}

void ButtonGuide::Draw() {
  if (!s_isVisible) return;

  InstallerFont* font = InstallerAssets::BodyFont();

  ImVec2 regionMin = {g_aspectRatioOffsetX + Scale(g_sideMargins),
                      g_aspectRatioOffsetY * 2.0f + Scale(720.0f - 102.0f)};
  ImVec2 regionMax = {g_aspectRatioOffsetX + Scale(1280.0f - g_sideMargins),
                      g_aspectRatioOffsetY * 2.0f + Scale(720.0f)};

  auto textMarginX = Scale(21.25f);
  auto iconMarginX = Scale(4);
  auto fontSize = Scale(21.8f);

  auto offsetLeft = 0.0f;
  auto offsetRight = 0.0f;

  // Draw left aligned icons.
  for (int i = 0; i < int(g_buttons.size()); i++) {
    auto& btn = g_buttons[i];

    if (btn.Alignment != EButtonAlignment::Left) continue;

    if (btn.Visibility && !*btn.Visibility) continue;

    auto str = Localise(btn.Name).c_str();
    auto iconWidth = Scale(g_iconWidths[btn.Icon]);
    auto iconHeight = Scale(g_iconHeights[btn.Icon]);
    auto textWidth = font->CalcTextSizeA(fontSize, FLT_MAX, 0, str).x;
    auto maxWidth = btn.MaxWidth == FLT_MAX ? textWidth : Scale(btn.MaxWidth);
    auto textScale = std::min(1.0f, maxWidth / textWidth);

    if (i > 0) offsetLeft += maxWidth + iconWidth + textMarginX;

    ImVec2 iconMin = {regionMin.x + offsetLeft - iconWidth - iconMarginX, regionMin.y};
    ImVec2 iconMax = {regionMin.x + offsetLeft - iconMarginX, regionMin.y + iconHeight};

    DrawGuide(&offsetLeft, regionMin, regionMax, btn.Icon, btn.Alignment, iconMin, iconMax,
              textWidth, maxWidth, textScale, fontSize, str);
  }

  // Draw right aligned icons.
  for (int i = int(g_buttons.size()) - 1; i >= 0; i--) {
    auto& btn = g_buttons[i];

    if (btn.Alignment != EButtonAlignment::Right) continue;

    if (btn.Visibility && !*btn.Visibility) continue;

    auto str = Localise(btn.Name).c_str();
    auto iconWidth = Scale(g_iconWidths[btn.Icon]);
    auto iconHeight = Scale(g_iconHeights[btn.Icon]);
    auto textWidth = font->CalcTextSizeA(fontSize, FLT_MAX, 0, str).x;
    auto maxWidth = btn.MaxWidth == FLT_MAX ? textWidth : Scale(btn.MaxWidth);
    auto textScale = std::min(1.0f, maxWidth / textWidth);

    if (i < int(g_buttons.size()) - 1) offsetRight += maxWidth + iconWidth + textMarginX;

    ImVec2 iconMin = {regionMax.x - offsetRight - iconWidth - iconMarginX, regionMin.y};
    ImVec2 iconMax = {regionMax.x - offsetRight - iconMarginX, regionMin.y + iconHeight};

    DrawGuide(&offsetRight, regionMin, regionMax, btn.Icon, btn.Alignment, iconMin, iconMax,
              textWidth, maxWidth, textScale, fontSize, str);
  }
}

void ButtonGuide::Open(Button button) {
  s_isVisible = true;
  g_sideMargins = DEFAULT_SIDE_MARGINS;

  g_buttons = {};
  g_buttons.push_back(button);
}

void ButtonGuide::Open(const std::span<Button> buttons) {
  s_isVisible = true;
  g_sideMargins = DEFAULT_SIDE_MARGINS;
  g_buttons = std::vector(buttons.begin(), buttons.end());
}

void ButtonGuide::SetSideMargins(float width) {
  g_sideMargins = width;
}

void ButtonGuide::Close() {
  s_isVisible = false;
}
