// Ported from UnleashedRecomp's ui/button_guide.cpp (hedge-dev, GPL-3.0).
// Changes: drawn like Sonic the Fighters' menus ("icon:label" from the bottom
// right, under the line across the screen), with the game's button glyphs.

#include "button_guide.h"

#include <algorithm>
#include <vector>

#include "imgui_utils.h"
#include "installer_pack.h"

constexpr float DEFAULT_SIDE_MARGINS = 379;

// Where the game's menus put it at 1280x720: the right edge and the middle.
constexpr float GUIDE_RIGHT = 1176.0f;
constexpr float GUIDE_CENTRE_Y = 650.0f;
constexpr float GUIDE_GAP = 28.0f;
constexpr float KBM_ICON_SIZE = 32.0f;

float g_sideMargins = DEFAULT_SIDE_MARGINS;

static std::vector<Button> g_buttons;

// The icon's texture, UVs and size in the 1280x720 layout.
static bool GetButtonIcon(EButtonIcon icon, GuestTexture*& texture, ImVec2& uvMin, ImVec2& uvMax,
                          ImVec2& size) {
  if (icon == EButtonIcon::LMB || icon == EButtonIcon::Enter || icon == EButtonIcon::Escape) {
    texture = InstallerAssets::Texture("kbm");
    int column = icon == EButtonIcon::LMB ? 0 : icon == EButtonIcon::Enter ? 1 : 2;
    auto uv = PIXELS_TO_UV_COORDS(384, 128, column * 128, 0, 128, 128);
    uvMin = std::get<0>(uv);
    uvMax = std::get<1>(uv);
    size = {KBM_ICON_SIZE, KBM_ICON_SIZE};
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
  if (!InstallerAssets::Button(id, texture, uvMin, uvMax, size)) return false;
  // The glyphs are the font's, at its 1920x1080 size.
  size = {size.x * 2.0f / 3.0f, size.y * 2.0f / 3.0f};
  return true;
}

void ButtonGuide::Draw() {
  if (!s_isVisible) return;

  InstallerFont* font = InstallerAssets::BodyFont();
  auto drawList = ImGui::GetBackgroundDrawList();
  auto fontSize = Scale(46.0f * 2.0f / 3.0f);
  float x = g_aspectRatioOffsetX + Scale(GUIDE_RIGHT);
  float centreY = g_aspectRatioOffsetY + Scale(GUIDE_CENTRE_Y);

  // The first button is the rightmost.
  for (auto& btn : g_buttons) {
    if (btn.Visibility && !*btn.Visibility) continue;

    std::string label = ":" + Localise(btn.Name);
    auto textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0, label.c_str());
    float maxWidth = btn.MaxWidth == FLT_MAX ? textSize.x : std::min(textSize.x, Scale(btn.MaxWidth));
    float textScale = maxWidth / std::max(textSize.x, 1.0f);

    x -= maxWidth;
    ImVec2 textPos = {x, centreY - textSize.y / 2};
    SetScale({textScale, 1.0f});
    SetOrigin(textPos);
    DrawTextBasic(font, fontSize, textPos, IM_COL32_WHITE, label.c_str());
    SetScale({1.0f, 1.0f});
    SetOrigin({0.0f, 0.0f});

    GuestTexture* texture;
    ImVec2 uvMin, uvMax, size;
    if (GetButtonIcon(btn.Icon, texture, uvMin, uvMax, size)) {
      ImVec2 iconSize = {Scale(size.x), Scale(size.y)};
      x -= iconSize.x;
      drawList->AddImage(TexRef(texture), {x, centreY - iconSize.y / 2},
                         {x + iconSize.x, centreY + iconSize.y / 2}, uvMin, uvMax);
    }
    x -= Scale(GUIDE_GAP);
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
