// Ported from UnleashedRecomp's ui/message_window.cpp (hedge-dev, GPL-3.0).
// Changes: input from InstallerInput rather than SDL listeners, installer-only
// (no in-game paths), the game's sounds and fonts.

#include "message_window.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "button_guide.h"
#include "imgui_utils.h"

constexpr double OVERLAY_CONTAINER_COMMON_MOTION_START = 0;
constexpr double OVERLAY_CONTAINER_COMMON_MOTION_END = 11;
constexpr double OVERLAY_CONTAINER_INTRO_FADE_START = 5;
constexpr double OVERLAY_CONTAINER_INTRO_FADE_END = 9;
constexpr double OVERLAY_CONTAINER_OUTRO_FADE_START = 0;
constexpr double OVERLAY_CONTAINER_OUTRO_FADE_END = 4;

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
      // Only accept mouse buttons when an item is selected.
      if (g_isControlsVisible && g_selectedRowIndex == -1) break;
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

static bool DrawContainer(float appearTime, ImVec2 centre, ImVec2 max, bool isForeground = true) {
  auto drawList = ImGui::GetBackgroundDrawList();

  ImVec2 _min = {centre.x - max.x, centre.y - max.y};
  ImVec2 _max = {centre.x + max.x, centre.y + max.y};

  // Expand/retract animation.
  auto containerMotion = float(ComputeMotion(appearTime, OVERLAY_CONTAINER_COMMON_MOTION_START,
                                             OVERLAY_CONTAINER_COMMON_MOTION_END));

  if (g_isClosing) {
    _min.x = Hermite(_min.x, centre.x, containerMotion);
    _max.x = Hermite(_max.x, centre.x, containerMotion);
    _min.y = Hermite(_min.y, centre.y, containerMotion);
    _max.y = Hermite(_max.y, centre.y, containerMotion);
  } else {
    _min.x = Hermite(centre.x, _min.x, containerMotion);
    _max.x = Hermite(centre.x, _max.x, containerMotion);
    _min.y = Hermite(centre.y, _min.y, containerMotion);
    _max.y = Hermite(centre.y, _max.y, containerMotion);
  }

  // Transparency fade animation.
  auto colourMotion = float(
      g_isClosing ? ComputeMotion(appearTime, OVERLAY_CONTAINER_OUTRO_FADE_START,
                                  OVERLAY_CONTAINER_OUTRO_FADE_END)
                  : ComputeMotion(appearTime, OVERLAY_CONTAINER_INTRO_FADE_START,
                                  OVERLAY_CONTAINER_INTRO_FADE_END));

  auto alpha = g_isClosing ? Lerp(1, 0, colourMotion) : Lerp(0, 1, colourMotion);

  if (!isForeground) g_foregroundCount++;

  if (isForeground)
    drawList->AddRectFilled({0.0f, 0.0f}, ImGui::GetIO().DisplaySize,
                            IM_COL32(0, 0, 0, int(190 * (g_foregroundCount ? 1 : alpha))));

  DrawPauseContainer(_min, _max, alpha);

  if (containerMotion >= 1.0f && !g_isClosing) {
    drawList->PushClipRect(_min, _max);
    return true;
  }

  return false;
}

static void DrawButton(int rowIndex, float yOffset, float width, float height, std::string& text) {
  auto drawList = ImGui::GetBackgroundDrawList();

  auto clipRectMin = drawList->GetClipRectMin();
  auto clipRectMax = drawList->GetClipRectMax();

  ImVec2 min = {clipRectMin.x + ((clipRectMax.x - clipRectMin.x) - width) / 2,
                clipRectMin.y + height * rowIndex + yOffset};
  ImVec2 max = {min.x + width, min.y + height};

  bool isSelected = rowIndex == g_selectedRowIndex;

  if (isSelected) {
    auto prevItemOffset = (g_prevSelectedRowIndex - g_selectedRowIndex) * height;
    auto animRatio = std::clamp((ImGui::GetTime() - g_rowSelectionTime) * 60.0 / 8.0, 0.0, 1.0);
    prevItemOffset *= float(pow(1.0 - animRatio, 3.0));

    DrawSelectionContainer({min.x, min.y + prevItemOffset}, {max.x, max.y + prevItemOffset}, true);
  }

  InstallerFont* font = InstallerAssets::BodyFont();
  auto fontSize = Scale(28);
  auto textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0, text.c_str());

  DrawTextWithShadow(font, fontSize,
                     {min.x + ((max.x - min.x) - textSize.x) / 2,
                      min.y + ((max.y - min.y) - textSize.y) / 2},
                     isSelected ? IM_COL32(255, 230, 40, 255) : IM_COL32(255, 255, 255, 255),
                     text.c_str());
}

static void DrawNextButtonGuide(bool isController, bool isKeyboard) {
  auto icon = isController ? EButtonIcon::A : isKeyboard ? EButtonIcon::Enter : EButtonIcon::LMB;

  ButtonGuide::Open(Button("Common_Next", FLT_MAX, icon));
}

static void ResetSelection() {
  /* Always use -1 for mouse input to prevent the selection
     cursor from erroneously appearing where it shouldn't. */
  g_selectedRowIndex = hid::g_inputDevice == hid::EInputDevice::Mouse ? -1 : g_defaultButtonIndex;

  g_upWasHeld = false;
  g_downWasHeld = false;
  g_joypadAxis = {};
  g_isAccepted = false;
  g_isDeclined = false;
}

void MessageWindow::Draw() {
  if (!s_isVisible) return;

  InstallerFont* font = InstallerAssets::BodyFont();
  auto drawList = ImGui::GetBackgroundDrawList();
  auto& res = ImGui::GetIO().DisplaySize;

  ImVec2 centre = {res.x / 2, res.y / 2};

  auto maxWidth = Scale(820);
  auto fontSize = Scale(28);

  const auto input = RemoveRubyAnnotations(g_text.c_str());
  auto lines = Split(input.first.c_str(), font, fontSize, maxWidth);

  for (auto& line : lines) {
    line = ReAddRubyAnnotations(line, input.second);
  }

  auto lineMargin = Config::Language != ELanguage::Japanese ? 5.0f : 5.5f;
  auto textSize = MeasureCentredParagraph(font, fontSize, lineMargin, lines);
  auto textMarginX = Scale(37);
  auto textMarginY = Scale(45);

  auto textX = centre.x;
  auto textY = centre.y + Scale(3);

  if (Config::Language == ELanguage::Japanese) {
    textMarginX -= Scale(2.5f);
    textMarginY -= Scale(2.0f);

    textY += Scale(lines.size() % 2 == 0 ? 1.5f : 8.0f);
  }

  bool isController = hid::IsInputDeviceController();
  bool isKeyboard = hid::g_inputDevice == hid::EInputDevice::Keyboard;

  if (DrawContainer(float(g_appearTime), centre,
                    {textSize.x / 2 + textMarginX, textSize.y / 2 + textMarginY},
                    !g_isControlsVisible)) {
    DrawRubyAnnotatedText(
        font, fontSize, maxWidth, {textX, textY}, lineMargin, g_text.c_str(),

        [=](const char* str, ImVec2 pos) {
          DrawTextWithShadow(font, fontSize, pos, IM_COL32(255, 255, 255, 255), str);
        },
        [=](const char* str, float size, ImVec2 pos) {
          DrawTextWithShadow(font, size, pos, IM_COL32(255, 255, 255, 255), str, 1.5f, 1.5f);
        },

        true);

    drawList->PopClipRect();

    if (g_buttons.size()) {
      auto itemWidth = std::max(Scale(162), Scale(CalcWidestTextSize(font, fontSize, g_buttons)));
      auto itemHeight = Scale(57);
      auto windowMarginX = Scale(23);
      auto windowMarginY = Scale(30);

      ImVec2 controlsMax = {itemWidth / 2 + windowMarginX,
                            itemHeight / 2 * g_buttons.size() + windowMarginY};

      if (g_isControlsVisible && DrawContainer(float(g_controlsAppearTime), centre, controlsMax)) {
        auto rowCount = 0;

        for (auto& button : g_buttons) DrawButton(rowCount++, windowMarginY, itemWidth, itemHeight, button);

        if (isController || isKeyboard) {
          bool upIsHeld = g_joypadAxis.y > 0.5f;
          bool downIsHeld = g_joypadAxis.y < -0.5f;

          bool scrollUp = !g_upWasHeld && upIsHeld;
          bool scrollDown = !g_downWasHeld && downIsHeld;

          auto prevSelectedRowIndex = g_selectedRowIndex;

          if (scrollUp) {
            --g_selectedRowIndex;
            if (g_selectedRowIndex < 0) g_selectedRowIndex = rowCount - 1;
          } else if (scrollDown) {
            ++g_selectedRowIndex;
            if (g_selectedRowIndex >= rowCount) g_selectedRowIndex = 0;
          }

          if (scrollUp || scrollDown) {
            Game_PlaySound(InstallerSound::Cursor);
            g_rowSelectionTime = ImGui::GetTime();
            g_prevSelectedRowIndex = prevSelectedRowIndex;
            g_joypadAxis.y = 0;
          }

          g_upWasHeld = upIsHeld;
          g_downWasHeld = downIsHeld;

          auto selectIcon = EButtonIcon::A;
          auto backIcon = EButtonIcon::B;

          if (isKeyboard) {
            selectIcon = EButtonIcon::Enter;
            backIcon = EButtonIcon::Escape;
          }

          std::array<Button, 2> buttons = {
              Button("Common_Select", 115.0f, selectIcon),
              Button("Common_Back", FLT_MAX, backIcon),
          };

          ButtonGuide::Open(buttons);

          if (g_isDeclined) {
            g_result = g_cancelButtonIndex;

            Game_PlaySound(InstallerSound::Cancel);
            MessageWindow::Close();
          }
        } else {
          auto clipRectMin = drawList->GetClipRectMin();
          auto clipRectMax = drawList->GetClipRectMax();

          ImVec2 listMin = {clipRectMin.x + windowMarginX, clipRectMin.y + windowMarginY};
          ImVec2 listMax = {clipRectMax.x - windowMarginX,
                            clipRectMin.y + windowMarginY + itemHeight * rowCount};

          // Invalidate index if the mouse cursor is outside of the list box.
          if (!ImGui::IsMouseHoveringRect(listMin, listMax, false)) g_selectedRowIndex = -1;

          for (int i = 0; i < rowCount; i++) {
            ImVec2 itemMin = {listMin.x, listMin.y + itemHeight * i};
            ImVec2 itemMax = {listMax.x, clipRectMin.y + windowMarginY + itemHeight * i + itemHeight};

            if (ImGui::IsMouseHoveringRect(itemMin, itemMax, false)) {
              if (g_selectedRowIndex != i) Game_PlaySound(InstallerSound::Cursor);

              g_selectedRowIndex = i;

              break;
            }
          }

          std::array<Button, 2> buttons = {
              Button("Common_Select", 115.0f, EButtonIcon::LMB),
              Button("Common_Back", FLT_MAX, EButtonIcon::Escape),
          };

          ButtonGuide::Open(buttons);

          if (g_isDeclined) {
            g_result = g_cancelButtonIndex;

            Game_PlaySound(InstallerSound::Cancel);
            MessageWindow::Close();
          }
        }

        if (g_selectedRowIndex != -1 && g_isAccepted) {
          g_result = g_selectedRowIndex;

          Game_PlaySound(InstallerSound::Decide);
          MessageWindow::Close();
        }

        drawList->PopClipRect();
      } else {
        DrawNextButtonGuide(isController, isKeyboard);

        if (!g_isControlsVisible && g_isAccepted) {
          g_controlsAppearTime = ImGui::GetTime();
          g_isControlsVisible = true;

          ResetSelection();
          Game_PlaySound(InstallerSound::Ring);
        }
      }
    } else {
      DrawNextButtonGuide(isController, isKeyboard);

      if (g_isAccepted) {
        g_result = 0;

        MessageWindow::Close();
      }
    }
  } else if (g_isClosing) {
    s_isVisible = false;
  }

  // Input is consumed once per frame.
  g_isAccepted = false;
  g_isDeclined = false;
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
