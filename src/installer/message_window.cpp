// Ported from UnleashedRecomp's ui/message_window.cpp (hedge-dev, GPL-3.0).
// Changes: drawn like Sonic the Fighters' message windows (the text, a line,
// then the choices, with an OK when there are none), input from
// InstallerInput rather than SDL listeners, installer-only (no in-game paths),
// the game's sounds and fonts.

#include "message_window.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "button_guide.h"
#include "imgui_utils.h"

static bool g_isAwaitingResult = false;
// Set when the window closes until its result is returned by Open.
static bool g_isResultPending = false;
static bool g_isClosing = false;
static bool g_isControlsVisible = false;

static double g_rowSelectionTime;
static int g_selectedRowIndex;
static int g_prevSelectedRowIndex;
static int g_foregroundCount;

static bool g_upWasHeld;
static bool g_downWasHeld;

static ImVec2 g_joypadAxis = {};
static bool g_isAccepted;
static bool g_isDeclined;
static bool g_isMouseClick;

static double g_appearTime;
static double g_controlsAppearTime;

static std::string g_text;
static int g_result;
static std::vector<std::string> g_buttons;
static int g_defaultButtonIndex;
static int g_cancelButtonIndex;

void MessageWindow::HandleInput(const InstallerInputEvent& event) {
  if (!s_isVisible) return;

  using Event = InstallerInputEvent;
  constexpr float axisTapRange = 0.5f;

  switch (event.type) {
    case Event::Type::KeyDown:
      switch (event.code) {
        case Event::KeyUp: g_joypadAxis.y = -1.0f; break;
        case Event::KeyDown: g_joypadAxis.y = 1.0f; break;
        case Event::KeyReturn: g_isAccepted = true; break;
        case Event::KeyEscape: g_isDeclined = true; break;
      }
      break;

    case Event::Type::MouseButtonDown:
      // Only accepted over a choice (checked when drawing).
      g_isMouseClick = true;
      g_isAccepted = true;
      break;

    case Event::Type::ControllerButton:
      switch (event.code) {
        case Event::DpadUp: g_joypadAxis = {0.0f, -1.0f}; break;
        case Event::DpadDown: g_joypadAxis = {0.0f, 1.0f}; break;
        case Event::ButtonA: g_isAccepted = true; break;
        case Event::ButtonB: g_isDeclined = true; break;
      }
      break;

    case Event::Type::ControllerAxis:
      if (event.code < 2) {
        float newAxisValue = event.value;
        float& axis = event.code ? g_joypadAxis.y : g_joypadAxis.x;
        bool sameDirection = (newAxisValue * axis) > 0.0f;
        bool wasInRange = std::abs(axis) > axisTapRange;
        bool isInRange = std::abs(newAxisValue) > axisTapRange;
        (void)sameDirection;
        (void)wasInRange;
        (void)isInRange;
        axis = newAxisValue;
      }
      break;

    default:
      break;
  }
}

// Layout at 1280x720, like the game's message windows (its save notice): the
// text, a white line, then the choices.
constexpr float WINDOW_X0 = 255.0f;
constexpr float WINDOW_X1 = 1026.0f;
constexpr float WINDOW_CENTRE_Y = 262.0f;
constexpr float WINDOW_TOP = 85.0f;
constexpr float TEXT_MARGIN_X = 40.0f;
constexpr float TEXT_TOP = 33.0f;
constexpr float ROW_HEIGHT = 36.0f;
constexpr float BOTTOM_PADDING = 23.0f;
constexpr float BODY_FONT_SIZE = 46.0f * 2.0f / 3.0f;
constexpr float LINE_HEIGHT = 36.0f;
constexpr double FADE_DURATION = 0.15;

static void ResetSelection() {
  // The game's menus always show a selection.
  g_selectedRowIndex = g_defaultButtonIndex;

  g_upWasHeld = false;
  g_downWasHeld = false;
  g_joypadAxis = {};
  g_isAccepted = false;
  g_isDeclined = false;
  g_isMouseClick = false;
}

void MessageWindow::Draw() {
  if (!s_isVisible) return;

  InstallerFont* font = InstallerAssets::BodyFont();
  auto drawList = ImGui::GetBackgroundDrawList();
  auto layout = [](float x, float y) {
    return ImVec2(g_aspectRatioOffsetX + Scale(x), g_aspectRatioOffsetY + Scale(y));
  };

  double motion = std::clamp((ImGui::GetTime() - g_appearTime) / FADE_DURATION, 0.0, 1.0);
  if (g_isClosing && motion >= 1.0) {
    s_isVisible = false;
    g_isAccepted = false;
    g_isDeclined = false;
    g_isMouseClick = false;
    return;
  }
  float alpha = float(g_isClosing ? 1.0 - motion : motion);
  bool isReady = !g_isClosing && motion >= 1.0;

  // Without choices, the window has an OK.
  std::vector<std::string> rows = g_buttons;
  if (rows.empty()) rows.push_back(Localise("Common_OK"));
  int rowCount = int(rows.size());

  auto fontSize = Scale(BODY_FONT_SIZE);
  auto lineMargin = LINE_HEIGHT - BODY_FONT_SIZE + (Config::Language == ELanguage::Japanese ? 1.0f : 0.0f);
  auto textWidth = Scale(WINDOW_X1 - WINDOW_X0 - TEXT_MARGIN_X * 2);
  float textHeight = MeasureCentredParagraph(font, fontSize, textWidth, lineMargin, g_text.c_str()).y /
                     g_aspectRatioScale;
  float height = TEXT_TOP + textHeight + 14.0f + 3.0f + 17.0f + rowCount * ROW_HEIGHT + BOTTOM_PADDING;
  // Centred where the game's are, from the top of the menus' window when tall.
  float y0 = std::max(WINDOW_TOP, WINDOW_CENTRE_Y - height / 2);
  // Opens out from the middle.
  float grow = float(g_isClosing ? 1.0 : 0.85 + 0.15 * Hermite(0, 1, float(motion)));
  float centreY = y0 + height / 2;

  ImVec2 bodyMin = layout(WINDOW_X0, centreY - height / 2 * grow);
  ImVec2 bodyMax = layout(WINDOW_X1, centreY + height / 2 * grow);
  DrawStfPanel(bodyMin, bodyMax, alpha);
  DrawStfTab(layout(WINDOW_X0 - 9.0f, centreY - height / 2 * grow - 9.0f), alpha);
  if (!isReady && grow < 1.0f) return;

  bool japanese = Config::Language == ELanguage::Japanese;
  ImVec2 textPos = layout(WINDOW_X0 + TEXT_MARGIN_X, y0 + TEXT_TOP);
  if (japanese) textPos.y += fontSize * ANNOTATION_FONT_SIZE_MODIFIER * 0.8f;
  DrawRubyAnnotatedText(
      font, fontSize, textWidth, textPos, lineMargin, g_text.c_str(),
      [=](const char* str, ImVec2 pos) {
        DrawTextBasic(font, fontSize, pos, IM_COL32(255, 255, 255, int(255 * alpha)), str);
      },
      [=](const char* str, float size, ImVec2 pos) {
        DrawTextBasic(font, size, pos, IM_COL32(255, 255, 255, int(255 * alpha)), str);
      },
      false, japanese);

  float y = y0 + TEXT_TOP + textHeight + 14.0f;
  DrawStfRule(bodyMin.x, bodyMax.x, layout(0, y).y, alpha);
  y += 3.0f + 17.0f;

  bool isController = hid::IsInputDeviceController();
  bool isKeyboard = hid::g_inputDevice == hid::EInputDevice::Keyboard;

  // Choices.
  int hovered = -1;
  if (isReady) {
    if (isController || isKeyboard) {
      bool upIsHeld = g_joypadAxis.y < -0.5f;
      bool downIsHeld = g_joypadAxis.y > 0.5f;
      bool scrollUp = !g_upWasHeld && upIsHeld;
      bool scrollDown = !g_downWasHeld && downIsHeld;
      if (g_selectedRowIndex < 0 && (scrollUp || scrollDown)) {
        g_selectedRowIndex = g_defaultButtonIndex;
      } else if (scrollUp) {
        g_selectedRowIndex = (g_selectedRowIndex + rowCount - 1) % rowCount;
      } else if (scrollDown) {
        g_selectedRowIndex = (g_selectedRowIndex + 1) % rowCount;
      }
      if ((scrollUp || scrollDown) && rowCount > 1) {
        Game_PlaySound(InstallerSound::Cursor);
        g_rowSelectionTime = ImGui::GetTime();
      }
      g_upWasHeld = upIsHeld;
      g_downWasHeld = downIsHeld;
      g_joypadAxis.y = 0;
      if (g_selectedRowIndex < 0) g_selectedRowIndex = g_defaultButtonIndex;
    } else {
      // The mouse selects the choice under it, and the selection stays when
      // it leaves them.
      for (int i = 0; i < rowCount; ++i) {
        if (ImGui::IsMouseHoveringRect(layout(WINDOW_X0, y + i * ROW_HEIGHT),
                                       layout(WINDOW_X1, y + (i + 1) * ROW_HEIGHT), false)) {
          hovered = i;
        }
      }
      if (hovered >= 0 && hovered != g_selectedRowIndex) {
        Game_PlaySound(InstallerSound::Cursor);
        g_selectedRowIndex = hovered;
      }
    }
  }

  for (int i = 0; i < rowCount; ++i) {
    ImVec2 min = {bodyMin.x + Scale(1), layout(0, y + i * ROW_HEIGHT).y};
    ImVec2 max = {bodyMax.x - Scale(1), layout(0, y + (i + 1) * ROW_HEIGHT).y};
    if (i % 2 == 0) drawList->AddRectFilled(min, max, IM_COL32(0, 51, 125, int(64 * alpha)));
    if (i == g_selectedRowIndex) DrawSelectionContainer(min, max, alpha);
    auto size = font->CalcTextSizeA(fontSize, FLT_MAX, 0, rows[i].c_str());
    DrawTextBasic(font, fontSize, {(min.x + max.x - size.x) / 2, (min.y + max.y - size.y) / 2},
                  IM_COL32(255, 255, 255, int(255 * alpha)), rows[i].c_str());
  }

  if (isReady) {
    auto selectIcon = isController ? EButtonIcon::A : isKeyboard ? EButtonIcon::Enter : EButtonIcon::LMB;
    auto backIcon = isController ? EButtonIcon::B : EButtonIcon::Escape;
    if (g_buttons.empty()) {
      ButtonGuide::Open(Button("Common_Select", FLT_MAX, selectIcon));
    } else {
      std::array<Button, 2> buttons = {Button("Common_Select", FLT_MAX, selectIcon),
                                       Button("Common_Back", FLT_MAX, backIcon)};
      ButtonGuide::Open(buttons);
    }

    bool clickMissed = g_isMouseClick && hovered < 0;
    if (g_isAccepted && !clickMissed && g_selectedRowIndex >= 0) {
      g_result = g_buttons.empty() ? 0 : g_selectedRowIndex;
      Game_PlaySound(InstallerSound::Decide);
      MessageWindow::Close();
    } else if (g_isDeclined) {
      g_result = g_buttons.empty() ? 0 : g_cancelButtonIndex;
      Game_PlaySound(InstallerSound::Cancel);
      MessageWindow::Close();
    }
  }

  // Input is consumed once per frame.
  g_isAccepted = false;
  g_isDeclined = false;
  g_isMouseClick = false;
}

bool MessageWindow::Open(std::string text, int* result, std::span<std::string> buttons,
                         int defaultButtonIndex, int cancelButtonIndex) {
  if (g_isResultPending) {
    // Returns the result of the window that just closed instead of opening it again.
    g_isResultPending = false;
    *result = g_result;
    return true;
  }

  if (!g_isAwaitingResult && *result == -1) {
    s_isVisible = true;
    g_isClosing = false;
    g_isControlsVisible = false;
    g_foregroundCount = 0;
    g_appearTime = ImGui::GetTime();
    g_controlsAppearTime = ImGui::GetTime();

    g_text = text;
    g_buttons = std::vector(buttons.begin(), buttons.end());
    g_defaultButtonIndex = defaultButtonIndex;
    g_cancelButtonIndex = cancelButtonIndex;
    g_result = -1;

    ResetSelection();

    Game_PlaySound(InstallerSound::Ring);

    g_isAwaitingResult = true;
  }

  *result = g_result;

  // Returns true when the message window is closed.
  return !g_isAwaitingResult;
}

void MessageWindow::Close() {
  if (!g_isClosing) {
    g_appearTime = ImGui::GetTime();
    g_controlsAppearTime = ImGui::GetTime();
    g_isClosing = true;
    g_isControlsVisible = false;
    g_foregroundCount = 0;
    g_isResultPending = g_isAwaitingResult;
    g_isAwaitingResult = false;

    ButtonGuide::Close();
  }
}
