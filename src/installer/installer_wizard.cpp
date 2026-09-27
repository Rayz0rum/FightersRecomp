// Ported from UnleashedRecomp's ui/installer_wizard.cpp (hedge-dev, GPL-3.0).
// Changes: Sonic the Fighters' pages (its Xbox 360 World package, no title
// update or DLC), art, fonts, sounds and colours; input from InstallerInput;
// the Windows file dialog; drawn a frame at a time by the app's dialog.

#include "installer_wizard.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <list>
#include <memory>
#include <sstream>
#include <thread>

#include "button_guide.h"
#include "imgui_utils.h"
#include "installer.h"
#include "installer_audio.h"
#include "installer_common.h"
#include "installer_input.h"
#include "message_window.h"

const char* g_versionString = "Sonic the Fighters Recompiled DX";

// One Shot Animations Constants
static constexpr double SCANLINES_ANIMATION_TIME = 0.0;
static constexpr double SCANLINES_ANIMATION_DURATION = 15.0;

static constexpr double MILES_ICON_ANIMATION_TIME = SCANLINES_ANIMATION_TIME + 10.0;
static constexpr double MILES_ICON_ANIMATION_DURATION = 15.0;

static constexpr double IMAGE_ANIMATION_TIME = MILES_ICON_ANIMATION_TIME + MILES_ICON_ANIMATION_DURATION;
static constexpr double IMAGE_ANIMATION_DURATION = 15.0;

static constexpr double TITLE_ANIMATION_TIME = SCANLINES_ANIMATION_DURATION;
static constexpr double TITLE_ANIMATION_DURATION = 30.0;

static constexpr double CONTAINER_LINE_ANIMATION_TIME = SCANLINES_ANIMATION_DURATION;
static constexpr double CONTAINER_LINE_ANIMATION_DURATION = 23.0;

static constexpr double CONTAINER_OUTER_TIME = SCANLINES_ANIMATION_DURATION + CONTAINER_LINE_ANIMATION_DURATION;
static constexpr double CONTAINER_OUTER_DURATION = 23.0;

static constexpr double CONTAINER_INNER_TIME = SCANLINES_ANIMATION_DURATION + CONTAINER_LINE_ANIMATION_DURATION + 8.0;
static constexpr double CONTAINER_INNER_DURATION = 15.0;

static constexpr double ALL_ANIMATIONS_FULL_DURATION = CONTAINER_INNER_TIME + CONTAINER_INNER_DURATION;
static constexpr double QUITTING_EXTRA_DURATION = 60.0;

static constexpr double INSTALL_ICONS_FADE_IN_ANIMATION_TIME = 0.0;
static constexpr double INSTALL_ICONS_FADE_IN_ANIMATION_DURATION = 15.0;

// Loop Animations Constants - their time range is [0.0, 1.0 + DELAY]
static constexpr double ARROW_CIRCLE_LOOP_SPEED = 1;

static constexpr double PULSE_ANIMATION_LOOP_SPEED = 1.5;
static constexpr double PULSE_ANIMATION_LOOP_DELAY = 0.5;
static constexpr double PULSE_ANIMATION_LOOP_FADE_HIGH_POINT = 0.5;

constexpr float IMAGE_X = 161.5f;
constexpr float IMAGE_Y = 103.5f;
constexpr float IMAGE_WIDTH = 512.0f;
constexpr float IMAGE_HEIGHT = 512.0f;

constexpr float CONTAINER_X = 513.0f;
constexpr float CONTAINER_Y = 226.0f;
constexpr float CONTAINER_WIDTH = 526.5f;
constexpr float CONTAINER_HEIGHT = 246.0f;
constexpr float SIDE_CONTAINER_WIDTH = CONTAINER_WIDTH / 2.0f;

constexpr float BOTTOM_X_GAP = 4.0f;
constexpr float BOTTOM_Y_GAP = 5.0f;
constexpr float CONTAINER_BUTTON_WIDTH = 250.0f;
constexpr float CONTAINER_BUTTON_GAP = 9.0f;
constexpr float BUTTON_HEIGHT = 22.0f;
constexpr float BUTTON_TEXT_GAP = 28.0f;

constexpr float BORDER_SIZE = 1.0f;
constexpr float BORDER_OVERSHOOT = 36.0f;

static constexpr size_t GRID_SIZE = 9;


static InstallerFont* g_bodyFont;
static InstallerFont* g_titleFont;

static double g_appearTime = 0.0;
static double g_disappearTime = DBL_MAX;
static bool g_isDisappearing = false;
static bool g_isQuitting = false;

static std::filesystem::path g_installPath;
static std::filesystem::path g_gameSourcePath;
static std::array<GuestTexture*, 10> g_installTextures;
static GuestTexture* g_sonicIcon;
static GuestTexture* g_loadingArc;
static GuestTexture* g_ringGlow;
static GuestTexture* g_projectLogo;
static Journal g_installerJournal;
static Installer::Sources g_installerSources;
static uint64_t g_installerAvailableSize = 0;
static std::unique_ptr<std::thread> g_installerThread;
static double g_installerStartTime = 0.0;
static double g_installerEndTime = DBL_MAX;
static float g_installerProgressRatioCurrent = 0.0f;
static std::atomic<float> g_installerProgressRatioTarget = 0.0f;
static std::atomic<bool> g_installerFinished = false;
static std::atomic<bool> g_installerHalted = false;
static std::atomic<bool> g_installerCancelled = false;
static bool g_installerFailed = false;
static std::string g_installerErrorMessage;

enum class WizardPage {
  SelectLanguage,
  Introduction,
  SelectGame,
  CheckSpace,
  Installing,
  InstallSucceeded,
  InstallFailed,
};

enum class MessagePromptSource { Unknown, Next, Back };

static WizardPage g_firstPage = WizardPage::SelectLanguage;
static WizardPage g_currentPage = g_firstPage;
static std::string g_currentMessagePrompt = "";
static MessagePromptSource g_currentMessagePromptSource = MessagePromptSource::Unknown;
static bool g_currentMessagePromptConfirmation = false;
static std::list<std::filesystem::path> g_currentPickerResults;
static std::atomic<bool> g_currentPickerResultsReady = false;
static std::string g_currentPickerErrorMessage;
static std::unique_ptr<std::thread> g_currentPickerThread;
static bool g_pickerTutorialCleared[2] = {};
static bool g_pickerTutorialTriggered = false;
static bool g_pickerTutorialFolderMode = false;
static bool g_currentPickerVisible = false;
static bool g_currentPickerFolderMode = false;
static HWND g_pickerOwner = nullptr;
static int g_currentMessageResult = -1;
static ImVec2 g_joypadAxis = {};
static int g_currentCursorIndex = -1;
static int g_currentCursorDefault = 0;
static bool g_currentCursorAccepted = false;
static bool g_currentCursorBack = false;
static std::vector<std::pair<ImVec2, ImVec2>> g_currentCursorRects;
static std::string g_creditsStr;

static void HandleInput(const InstallerInputEvent& event) {
  using Event = InstallerInputEvent;
  if (!InstallerWizard::s_isVisible) return;

  bool noModals = g_currentMessagePrompt.empty() && !g_currentPickerVisible;
  if (event.type == Event::Type::Quit && g_currentPage == WizardPage::Installing) {
    // Pretend the back button was pressed if the user tried quitting during installation.
    if (noModals) g_currentCursorBack = true;
    return;
  }

  if (!noModals) return;

  constexpr float AxisTapRange = 0.5f;
  int newCursorIndex = -1;
  ImVec2 tapDirection = {};

  switch (event.type) {
    case Event::Type::KeyDown: {
      switch (event.code) {
        case Event::KeyLeft:
        case Event::KeyRight:
          tapDirection.x = (event.code == Event::KeyRight) ? 1.0f : -1.0f;
          break;
        case Event::KeyUp:
        case Event::KeyDown:
          tapDirection.y = (event.code == Event::KeyDown) ? 1.0f : -1.0f;
          break;
        case Event::KeyReturn:
          g_currentCursorAccepted = (g_currentCursorIndex >= 0);
          break;
        case Event::KeyEscape:
          g_currentCursorBack = true;
          break;
      }
      break;
    }

    case Event::Type::ControllerButton: {
      switch (event.code) {
        case Event::DpadLeft: tapDirection = {-1.0f, 0.0f}; break;
        case Event::DpadRight: tapDirection = {1.0f, 0.0f}; break;
        case Event::DpadUp: tapDirection = {0.0f, -1.0f}; break;
        case Event::DpadDown: tapDirection = {0.0f, 1.0f}; break;
        case Event::ButtonA: g_currentCursorAccepted = (g_currentCursorIndex >= 0); break;
        case Event::ButtonB: g_currentCursorBack = true; break;
      }
      break;
    }

    case Event::Type::ControllerAxis: {
      if (event.code < 2) {
        float newAxisValue = event.value;
        float& axis = event.code ? g_joypadAxis.y : g_joypadAxis.x;
        bool sameDirection = (newAxisValue * axis) > 0.0f;
        bool wasInRange = std::abs(axis) > AxisTapRange;
        bool isInRange = std::abs(newAxisValue) > AxisTapRange;
        if (sameDirection && !wasInRange && isInRange) {
          (event.code ? tapDirection.y : tapDirection.x) = newAxisValue;
        }
        // A new press from rest counts too.
        if (!wasInRange && isInRange && axis == 0.0f) {
          (event.code ? tapDirection.y : tapDirection.x) = newAxisValue;
        }
        axis = newAxisValue;
      }
      break;
    }

    case Event::Type::MouseButtonDown:
    case Event::Type::MouseMotion: {
      for (size_t i = 0; i < g_currentCursorRects.size(); i++) {
        auto& currentRect = g_currentCursorRects[i];

        if (ImGui::IsMouseHoveringRect(currentRect.first, currentRect.second, false)) {
          newCursorIndex = int(i);

          if (event.type == Event::Type::MouseButtonDown) g_currentCursorAccepted = true;

          break;
        }
      }

      if (newCursorIndex < 0) g_currentCursorIndex = -1;

      break;
    }

    default:
      break;
  }

  if (tapDirection.x != 0.0f || tapDirection.y != 0.0f) {
    if (g_currentCursorIndex >= int(g_currentCursorRects.size()) || g_currentCursorIndex < 0) {
      newCursorIndex = g_currentCursorDefault;
    } else {
      auto& currentRect = g_currentCursorRects[g_currentCursorIndex];
      ImVec2 currentPoint = ImVec2(
          (currentRect.first.x + currentRect.second.x) / 2.0f +
              tapDirection.x * (currentRect.second.x - currentRect.first.x) / 2.0f,
          (currentRect.first.y + currentRect.second.y) / 2.0f +
              tapDirection.y * (currentRect.second.y - currentRect.first.y) / 2.0f);

      float closestDistance = FLT_MAX;
      for (size_t i = 0; i < g_currentCursorRects.size(); i++) {
        if (g_currentCursorIndex == int(i)) {
          continue;
        }

        auto& targetRect = g_currentCursorRects[i];
        ImVec2 targetPoint = ImVec2(
            (targetRect.first.x + targetRect.second.x) / 2.0f +
                tapDirection.x * (targetRect.first.x - targetRect.second.x) / 2.0f,
            (targetRect.first.y + targetRect.second.y) / 2.0f +
                tapDirection.y * (targetRect.first.y - targetRect.second.y) / 2.0f);

        ImVec2 delta = ImVec2(targetPoint.x - currentPoint.x, targetPoint.y - currentPoint.y);
        float projectedDistance = delta.x * tapDirection.x + delta.y * tapDirection.y;
        float manhattanDistance = std::abs(delta.x) + std::abs(delta.y);
        if (projectedDistance > 0.0f && manhattanDistance < closestDistance) {
          newCursorIndex = int(i);
          closestDistance = manhattanDistance;
        }
      }
    }
  }

  if (newCursorIndex >= 0) {
    if (g_currentCursorIndex != newCursorIndex) Game_PlaySound(InstallerSound::Cursor);

    g_currentCursorIndex = newCursorIndex;
  }
}

static const std::string& GetWizardText(WizardPage page) {
  switch (page) {
    case WizardPage::SelectLanguage: return Localise("Installer_Page_SelectLanguage");
    case WizardPage::Introduction: return Localise("Installer_Page_Introduction");
    case WizardPage::SelectGame: return Localise("Installer_Page_SelectGame");
    case WizardPage::CheckSpace: return Localise("Installer_Page_CheckSpace");
    case WizardPage::Installing: return Localise("Installer_Page_Installing");
    case WizardPage::InstallSucceeded: return Localise("Installer_Page_InstallSucceeded");
    case WizardPage::InstallFailed: return Localise("Installer_Page_InstallFailed");
  }

  return g_localeMissing;
}

// Sonic, Tails, Knuckles, Super Sonic, Eggman.
// The character shown on each page (installing starts with Espio and goes
// through them all).
static const int WIZARD_INSTALL_TEXTURE_INDEX[] = {
    0,  // Sonic
    1,  // Tails
    2,  // Knuckles
    3,  // Amy
    4,  // Espio
    0,  // Force Sonic on InstallSucceeded.
    9   // Force Robotnik on InstallFailed.
};

// These are ordered from bottom to top in a 3x2 grid.
const char* LANGUAGE_TEXT[] = {
    "FRANÇAIS",  // French
    "DEUTSCH",   // German
    "ENGLISH",   // English
    "ESPAÑOL",   // Spanish
    "ITALIANO",  // Italian
    "日本語",    // Japanese
};

const ELanguage LANGUAGE_ENUM[] = {
    ELanguage::French, ELanguage::German,  ELanguage::English,
    ELanguage::Spanish, ELanguage::Italian, ELanguage::Japanese,
};

static double ComputeMotionInstaller(double timeAppear, double timeDisappear, double offset,
                                     double total) {
  return ComputeMotion(timeAppear, offset, total) *
         (1.0 - ComputeMotion(timeDisappear, ALL_ANIMATIONS_FULL_DURATION - offset - total, total));
}

static double ComputeMotionInstallerLoop(double timeAppear, double speed, double offset) {
  return std::clamp(fmod((ImGui::GetTime() - timeAppear) * speed, 1.0 + offset) - offset, 0.0,
                    1.0) /
         1.0;
}

static double ComputeHermiteMotionInstallerLoop(double timeAppear, double speed, double offset) {
  return (cos(M_PI * ComputeMotionInstallerLoop(timeAppear, speed, offset) + M_PI) + 1) / 2;
}

static bool PushCursorRect(ImVec2 min, ImVec2 max, bool& cursorPressed, bool makeDefault = false) {
  int currentIndex = int(g_currentCursorRects.size());
  g_currentCursorRects.emplace_back(min, max);

  if (makeDefault) {
    g_currentCursorDefault = currentIndex;
  }

  if (g_currentCursorIndex == currentIndex) {
    if (g_currentCursorAccepted) {
      Game_PlaySound(InstallerSound::Decide);
      cursorPressed = true;
      g_currentCursorAccepted = false;
    }

    return true;
  } else {
    return false;
  }
}

static void ResetCursorRects() {
  g_currentCursorDefault = 0;
  g_currentCursorRects.clear();
}

static bool HasWideCharacters(const char* text) {
  for (const auto* s = reinterpret_cast<const unsigned char*>(text); *s; ++s) {
    if (*s >= 0xE3) return true;  // Kana and CJK (U+3000 and above).
  }
  return false;
}

static void DrawBackground() {
  // The deep blue of Sonic the Fighters' menus.
  auto& res = ImGui::GetIO().DisplaySize;
  auto drawList = ImGui::GetBackgroundDrawList();
  drawList->AddRectFilledMultiColor({0.0, 0.0}, res, IM_COL32(8, 26, 86, 255),
                                    IM_COL32(8, 26, 86, 255), IM_COL32(0, 4, 30, 255),
                                    IM_COL32(0, 4, 30, 255));
}

static void DrawLeftImage() {
  int installTextureIndex = WIZARD_INSTALL_TEXTURE_INDEX[int(g_currentPage)];
  if (g_currentPage == WizardPage::Installing) {
    // Cycle through the available images while time passes during installation.
    constexpr double InstallationSpeed = 1.0 / 15.0;
    double installationTime = (ImGui::GetTime() - g_installerStartTime) * InstallationSpeed;
    installTextureIndex += int(installationTime);
  }

  double imageAlpha = ComputeMotionInstaller(g_appearTime, g_disappearTime, IMAGE_ANIMATION_TIME,
                                             IMAGE_ANIMATION_DURATION);
  int a = int(std::lround(255.0 * imageAlpha));
  GuestTexture* guestTexture = g_installTextures[installTextureIndex % g_installTextures.size()];
  if (!guestTexture) return;
  auto drawList = ImGui::GetBackgroundDrawList();
  ImVec2 min = {g_aspectRatioOffsetX + Scale(IMAGE_X), g_aspectRatioOffsetY + Scale(IMAGE_Y)};
  ImVec2 max = {min.x + Scale(IMAGE_WIDTH), min.y + Scale(IMAGE_HEIGHT)};
  drawList->AddImage(TexRef(guestTexture), min, max, ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(255, 255, 255, a));
}

static void DrawHeaderIconsForInstallPhase(double iconsPosX, double iconsPosY, double iconsScale) {
  auto drawList = ImGui::GetBackgroundDrawList();

  // The game's loading arc, spinning.
  ImVec2 arrowCircleMin = {g_aspectRatioOffsetX + Scale(float(iconsPosX - iconsScale / 2)),
                           Scale(float(iconsPosY - iconsScale / 2))};
  ImVec2 arrowCircleMax = {g_aspectRatioOffsetX + Scale(float(iconsPosX + iconsScale / 2)),
                           Scale(float(iconsPosY + iconsScale / 2))};
  ImVec2 center = {g_aspectRatioOffsetX + Scale(float(iconsPosX)) + 0.5f,
                   Scale(float(iconsPosY)) - 0.5f};

  float arrowCircleFadeMotion = float(ComputeMotionInstaller(
      g_installerStartTime, g_installerEndTime, INSTALL_ICONS_FADE_IN_ANIMATION_TIME,
      INSTALL_ICONS_FADE_IN_ANIMATION_DURATION));
  float rotationMotion = float(ComputeMotionInstallerLoop(g_installerStartTime, ARROW_CIRCLE_LOOP_SPEED, 0));
  float rotation = float(-2 * M_PI * rotationMotion);

  // Calculate rotated corners
  float cosCurrentAngle = cosf(rotation);
  float sinCurrentAngle = sinf(rotation);
  auto rotate = [&](ImVec2 v) {
    return ImVec2(v.x * cosCurrentAngle - v.y * sinCurrentAngle,
                  v.x * sinCurrentAngle + v.y * cosCurrentAngle);
  };
  ImVec2 corners[4] = {
      rotate(ImVec2(arrowCircleMin.x - center.x, arrowCircleMin.y - center.y)),
      rotate(ImVec2(arrowCircleMax.x - center.x, arrowCircleMin.y - center.y)),
      rotate(ImVec2(arrowCircleMax.x - center.x, arrowCircleMax.y - center.y)),
      rotate(ImVec2(arrowCircleMin.x - center.x, arrowCircleMax.y - center.y)),
  };

  for (int i = 0; i < IM_ARRAYSIZE(corners); ++i) {
    corners[i].x += center.x;
    corners[i].y += center.y;
  }

  if (g_loadingArc) {
    drawList->AddImageQuad(TexRef(g_loadingArc), corners[0], corners[1], corners[2], corners[3],
                           ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1),
                           IM_COL32(120, 210, 255, int(200 * arrowCircleFadeMotion)));
  }

  // Pulse - a gold ring.
  float pulseFadeMotion = float(ComputeMotionInstaller(g_installerStartTime, g_installerEndTime,
                                                       INSTALL_ICONS_FADE_IN_ANIMATION_TIME,
                                                       INSTALL_ICONS_FADE_IN_ANIMATION_DURATION));
  float pulseMotion = float(ComputeMotionInstallerLoop(g_installerStartTime, PULSE_ANIMATION_LOOP_SPEED,
                                                       PULSE_ANIMATION_LOOP_DELAY));
  float pulseHermiteMotion = float(ComputeHermiteMotionInstallerLoop(
      g_installerStartTime, PULSE_ANIMATION_LOOP_SPEED, PULSE_ANIMATION_LOOP_DELAY));

  float pulseFade = float(pulseMotion / PULSE_ANIMATION_LOOP_FADE_HIGH_POINT);

  if (pulseMotion >= PULSE_ANIMATION_LOOP_FADE_HIGH_POINT) {
    // Calculate linear fade-out from high point time - ({PULSE_ANIMATION_LOOP_FADE_HIGH_POINT}, 1) - to loop end - (1, 0) -.
    float m = float(-1 / (1 - PULSE_ANIMATION_LOOP_FADE_HIGH_POINT));
    float b = float(m * (-PULSE_ANIMATION_LOOP_FADE_HIGH_POINT) + 1);

    pulseFade = m * pulseMotion + b;
  }

  float pulseScale = float(iconsScale * pulseHermiteMotion * 1.5);

  ImVec2 pulseMin = {g_aspectRatioOffsetX + Scale(float(iconsPosX - pulseScale / 2)),
                     Scale(float(iconsPosY - pulseScale / 2))};
  ImVec2 pulseMax = {g_aspectRatioOffsetX + Scale(float(iconsPosX + pulseScale / 2)),
                     Scale(float(iconsPosY + pulseScale / 2))};
  if (g_ringGlow) {
    SetAdditive(true);
    drawList->AddImage(TexRef(g_ringGlow), pulseMin, pulseMax, ImVec2(0, 0), ImVec2(1, 1),
                       IM_COL32(255, 200, 40, int(255 * pulseFade * pulseFadeMotion)));
    SetAdditive(false);
  }
}

static void DrawHeaderIcons() {
  auto drawList = ImGui::GetBackgroundDrawList();

  float iconsPosX = 256.0f;
  float iconsPosY = 80.0f;
  float iconsScale = 62.0f;

  // The little Sonic of the title screen (UnleashedRecomp: the Miles Electric icon).
  float milesIconMotion = float(ComputeMotionInstaller(g_appearTime, g_disappearTime,
                                                       MILES_ICON_ANIMATION_TIME,
                                                       MILES_ICON_ANIMATION_DURATION));
  float milesIconScale = iconsScale * (2 - milesIconMotion);

  if (g_sonicIcon) {
    float aspect = float(g_sonicIcon->width) / float(g_sonicIcon->height);
    float h = milesIconScale * 1.2f, w = h * aspect;
    ImVec2 milesElectricMin = {g_aspectRatioOffsetX + Scale(iconsPosX - w / 2),
                               Scale(iconsPosY - h / 2)};
    ImVec2 milesElectricMax = {g_aspectRatioOffsetX + Scale(iconsPosX + w / 2),
                               Scale(iconsPosY + h / 2)};
    drawList->AddImage(TexRef(g_sonicIcon), milesElectricMin, milesElectricMax, ImVec2(0, 0),
                       ImVec2(1, 1), IM_COL32(255, 255, 255, int(255 * milesIconMotion)));
  }

  if (int(g_currentPage) >= int(WizardPage::Installing)) {
    DrawHeaderIconsForInstallPhase(iconsPosX + 70.0f, iconsPosY, iconsScale);
  }
}

static void DrawScanlineBars() {
  double scanlinesAlpha = ComputeMotionInstaller(g_appearTime, g_disappearTime, 0.0,
                                                 SCANLINES_ANIMATION_DURATION);

  const uint32_t COLOR0 = IM_COL32(40, 150, 255, 0);
  const uint32_t COLOR1 = IM_COL32(40, 150, 255, int(55 * scanlinesAlpha));

  float height = Scale(105.0f) * float(ComputeMotionInstaller(g_appearTime, g_disappearTime, 0.0,
                                                              SCANLINES_ANIMATION_DURATION));
  if (height < 1e-6f) {
    return;
  }

  auto& res = ImGui::GetIO().DisplaySize;
  auto drawList = ImGui::GetBackgroundDrawList();

  SetShaderModifier(IMGUI_SHADER_MODIFIER_SCANLINE);

  // Top bar
  drawList->AddRectFilledMultiColor({0.0f, 0.0f}, {res.x, height}, COLOR0, COLOR0, COLOR1, COLOR1);

  // Bottom bar
  ImVec2 max{0.0f, res.y - height};
  SetProceduralOrigin(max);

  drawList->AddRectFilledMultiColor({res.x, res.y}, max, COLOR0, COLOR0, COLOR1, COLOR1);

  ResetProceduralOrigin();

  SetShaderModifier(IMGUI_SHADER_MODIFIER_NONE);

  // Installer text
  auto& headerText = Localise(g_currentPage == WizardPage::Installing ? "Installer_Header_Installing"
                                                                      : "Installer_Header_Installer");
  auto alphaMotion = ComputeMotionInstaller(g_appearTime, g_disappearTime, TITLE_ANIMATION_TIME,
                                            TITLE_ANIMATION_DURATION);
  auto breatheMotion = 1.0f;

  if (g_currentPage == WizardPage::Installing) {
    // Breathing animation
    static auto breatheStart = ImGui::GetTime();
    breatheMotion = float(BREATHE_MOTION(1.0f, 0.55f, breatheStart, 1.5f));
  }

  float titleX = int(g_currentPage) >= int(WizardPage::Installing) ? 358.0f : 288.0f;
  DrawTextWithOutline(g_titleFont, Scale(48.0f), {g_aspectRatioOffsetX + Scale(titleX), Scale(54.5f)},
                      IM_COL32(255, 222, 0, int(255 * alphaMotion * breatheMotion)),
                      headerText.c_str(), 4,
                      IM_COL32(0, 0, 0, int(255 * alphaMotion * breatheMotion)),
                      IMGUI_SHADER_MODIFIER_TITLE_BEVEL);

  auto drawLine = [&](bool top) {
    float y = top ? height : (res.y - height);

    const uint32_t TOP_COLOR0 = IM_COL32(190, 225, 255, int(7 * scanlinesAlpha));
    const uint32_t TOP_COLOR1 = IM_COL32(190, 225, 255, int(65 * scanlinesAlpha));
    const uint32_t BOTTOM_COLOR0 = IM_COL32(150, 200, 255, int(65 * scanlinesAlpha));
    const uint32_t BOTTOM_COLOR1 = IM_COL32(150, 200, 255, int(7 * scanlinesAlpha));

    drawList->AddRectFilledMultiColor({0.0f, y - Scale(2.0f)}, {res.x, y},
                                      top ? TOP_COLOR0 : BOTTOM_COLOR1,
                                      top ? TOP_COLOR0 : BOTTOM_COLOR1,
                                      top ? TOP_COLOR1 : BOTTOM_COLOR0,
                                      top ? TOP_COLOR1 : BOTTOM_COLOR0);

    drawList->AddRectFilledMultiColor({0.0f, y + Scale(1.0f)}, {res.x, y + Scale(3.0f)},
                                      top ? BOTTOM_COLOR0 : TOP_COLOR1,
                                      top ? BOTTOM_COLOR0 : TOP_COLOR1,
                                      top ? BOTTOM_COLOR1 : TOP_COLOR0,
                                      top ? BOTTOM_COLOR1 : TOP_COLOR0);

    const uint32_t CENTER_COLOR = IM_COL32(90, 150, 230, int(255 * scanlinesAlpha));
    drawList->AddRectFilled({0.0f, y}, {res.x, y + Scale(1.0f)}, CENTER_COLOR);
  };

  // Top bar line
  drawLine(true);

  // Bottom bar line
  drawLine(false);

  DrawHeaderIcons();
  DrawVersionString(g_bodyFont, IM_COL32(255, 255, 255, int(70 * alphaMotion)));
}

static void DrawContainer(ImVec2 min, ImVec2 max, bool isTextArea) {
  auto drawList = ImGui::GetBackgroundDrawList();

  double gridAlpha = ComputeMotionInstaller(g_appearTime, g_disappearTime,
                                            isTextArea ? CONTAINER_INNER_TIME : CONTAINER_OUTER_TIME,
                                            isTextArea ? CONTAINER_INNER_DURATION : CONTAINER_OUTER_DURATION);
  double gridOverlayAlpha = ComputeMotionInstaller(g_appearTime, g_disappearTime,
                                                   CONTAINER_INNER_TIME, CONTAINER_INNER_DURATION);

  const uint32_t gridColor = IM_COL32(0, 22, 60, int((isTextArea ? 223 : 255) * gridAlpha));
  const uint32_t gridOverlayColor = IM_COL32(0, 16, 50, int(128 * gridOverlayAlpha));

  float gridSize = Scale(GRID_SIZE);

  SetShaderModifier(IMGUI_SHADER_MODIFIER_CHECKERBOARD);
  SetAdditive(true);
  drawList->AddRectFilled(min, max, gridColor);
  SetAdditive(false);
  SetShaderModifier(IMGUI_SHADER_MODIFIER_NONE);

  if (isTextArea) {
    drawList->AddRectFilled(min, max, gridOverlayColor);
  }

  // The draw area
  drawList->PushClipRect({min.x - gridSize * 2.0f, min.y + gridSize * 2.0f},
                         {max.x - gridSize * 2.0f + 1.0f, max.y - gridSize * 2.0f + 1.0f});
}

static void DrawDescriptionContainer() {
  auto& res = ImGui::GetIO().DisplaySize;
  auto drawList = ImGui::GetBackgroundDrawList();
  auto fontSize = Scale(28.0f);
  auto annotationFontSize = fontSize * ANNOTATION_FONT_SIZE_MODIFIER;

  ImVec2 descriptionMin = {std::round(g_aspectRatioOffsetX + Scale(CONTAINER_X + 0.5f)),
                           std::round(g_aspectRatioOffsetY + Scale(CONTAINER_Y + 0.5f))};
  ImVec2 descriptionMax = {std::round(g_aspectRatioOffsetX + Scale(CONTAINER_X + 0.5f + CONTAINER_WIDTH)),
                           std::round(g_aspectRatioOffsetY + Scale(CONTAINER_Y + 0.5f + CONTAINER_HEIGHT))};
  SetProceduralOrigin(descriptionMin);
  DrawContainer(descriptionMin, descriptionMax, true);

  char descriptionText[1024];
  char requiredSpaceText[128];
  char availableSpaceText[128];
  snprintf(descriptionText, sizeof(descriptionText), "%s", GetWizardText(g_currentPage).c_str());

  if (g_currentPage == WizardPage::CheckSpace) {
    constexpr double DivisorGiB = (1024.0 * 1024.0 * 1024.0);
    double requiredGiB = double(g_installerSources.totalSize) / DivisorGiB;
    double availableGiB = double(g_installerAvailableSize) / DivisorGiB;
    snprintf(requiredSpaceText, sizeof(requiredSpaceText),
             Localise("Installer_Step_RequiredSpace").c_str(), requiredGiB);
    snprintf(availableSpaceText, sizeof(availableSpaceText),
             (g_installerAvailableSize > 0) ? Localise("Installer_Step_AvailableSpace").c_str() : "",
             availableGiB);
    snprintf(descriptionText, sizeof(descriptionText), "%s%s\n%s",
             GetWizardText(g_currentPage).c_str(), requiredSpaceText, availableSpaceText);
  } else if (g_currentPage == WizardPage::InstallFailed) {
    // Japanese needs text to be brought in by a normal width space
    // as it allows for text to begin further than others for
    // special characters.
    if (Config::Language == ELanguage::Japanese) {
      strncat(descriptionText, " ", sizeof(descriptionText) - strlen(descriptionText) - 1);
    }

    strncat(descriptionText, g_installerErrorMessage.c_str(),
            sizeof(descriptionText) - strlen(descriptionText) - 1);
  }

  double textAlpha = ComputeMotionInstaller(g_appearTime, g_disappearTime, CONTAINER_INNER_TIME,
                                            CONTAINER_INNER_DURATION);
  auto clipRectMin = drawList->GetClipRectMin();
  auto clipRectMax = drawList->GetClipRectMax();

  float textX = clipRectMin.x + fontSize;
  float textY = clipRectMin.y - Scale(1.0f);

  auto lineWidth = clipRectMax.x - (fontSize / 2.0f) - clipRectMin.x;

  clipRectMax.x += fontSize;
  clipRectMax.y += Scale(1.0f);

  float lineMargin = 5.0f;

  if (Config::Language == ELanguage::Japanese) {
    lineMargin = 5.5f;

    // Removing some padding of the applied due to the inclusion of annotation for Japanese
    textX -= (fontSize + Scale(1.5f));
    textY -= Scale(7.0f);

    // The annotation (and thus the Japanese) can be drawn above the edges of the info panel thus the clip needs to be extended a bit
    clipRectMin.x -= annotationFontSize;
    clipRectMin.y -= annotationFontSize;
    clipRectMax.x += annotationFontSize;
    clipRectMax.y += annotationFontSize;

    textX += annotationFontSize;
    textY += annotationFontSize;

    lineWidth += annotationFontSize;
  }

  drawList->PushClipRect(clipRectMin, clipRectMax, false);

  // Text longer than the panel scrolls by itself: it waits at the top, scrolls
  // down, waits at the bottom and starts over.
  static WizardPage scrollPage = WizardPage(-1);
  static ELanguage scrollLanguage = ELanguage::English;
  static double scrollStart = 0.0;
  if (scrollPage != g_currentPage || scrollLanguage != Config::Language) {
    scrollPage = g_currentPage;
    scrollLanguage = Config::Language;
    scrollStart = ImGui::GetTime();
  }
  float bottomMargin = g_currentPage == WizardPage::InstallSucceeded ? Scale(80) : Scale(6);
  float paragraphHeight =
      MeasureCentredParagraph(g_bodyFont, fontSize, lineWidth, lineMargin, descriptionText).y;
  float overflow = paragraphHeight - (clipRectMax.y - bottomMargin - textY);
  float scrollY = 0.0f;
  if (overflow > 0.0f) {
    constexpr double hold = 2.5;
    double speed = Scale(30.0f);
    double travel = overflow / speed;
    double t = fmod(ImGui::GetTime() - scrollStart, hold + travel + hold);
    scrollY = t < hold ? 0.0f : t < hold + travel ? float((t - hold) * speed) : overflow;
    float fade = Scale(24.0f);
    SetVerticalMarqueeFade(clipRectMin, {clipRectMax.x, clipRectMax.y - bottomMargin + Scale(6)},
                           scrollY > 0.0f ? fade : 0.001f, scrollY < overflow ? fade : 0.001f);
  }

  DrawRubyAnnotatedText(
      g_bodyFont, fontSize, lineWidth, {textX, textY - scrollY}, lineMargin, descriptionText,
      [=](const char* str, ImVec2 pos) {
        DrawTextBasic(g_bodyFont, fontSize, pos, IM_COL32(255, 255, 255, int(255 * textAlpha)), str);
      },
      [=](const char* str, float size, ImVec2 pos) {
        DrawTextBasic(g_bodyFont, size, pos, IM_COL32(255, 255, 255, int(255 * textAlpha)), str);
      },
      false, Config::Language == ELanguage::Japanese);

  if (overflow > 0.0f) {
    ResetMarqueeFade();
  }

  drawList->PopClipRect();
  drawList->PopClipRect();

  if (g_currentPage == WizardPage::InstallSucceeded) {
    auto descTextSize = MeasureCentredParagraph(g_bodyFont, fontSize, lineWidth, lineMargin, descriptionText);

    auto colWhite = IM_COL32(255, 255, 255, int(255 * textAlpha));

    auto containerLeft = g_aspectRatioOffsetX + Scale(CONTAINER_X);
    auto containerTop = g_aspectRatioOffsetY + Scale(CONTAINER_Y);
    auto containerRight = containerLeft + Scale(CONTAINER_WIDTH);
    auto containerBottom = containerTop + Scale(CONTAINER_HEIGHT);

    auto marqueeTextSize = g_bodyFont->CalcTextSizeA(fontSize, FLT_MAX, 0, g_creditsStr.c_str());
    auto marqueeTextMarginY = Scale(15);

    ImVec2 marqueeTextPos = {descriptionMax.x, containerBottom - marqueeTextSize.y - marqueeTextMarginY};
    ImVec2 marqueeTextMin = {containerLeft, marqueeTextPos.y};
    ImVec2 marqueeTextMax = {containerRight, containerBottom};

    // The project's logo (UnleashedRecomp: the hedge-dev logo and name).
    ImVec2 imageRegionMin = {containerLeft, textY + descTextSize.y};
    ImVec2 imageRegionMax = {containerRight, containerBottom - (marqueeTextMax.y - marqueeTextMin.y)};
    if (g_projectLogo) {
      float regionW = imageRegionMax.x - imageRegionMin.x, regionH = imageRegionMax.y - imageRegionMin.y;
      float logoH = std::min(regionH * 0.9f, regionW * 0.8f * g_projectLogo->height / g_projectLogo->width);
      float logoW = logoH * g_projectLogo->width / g_projectLogo->height;
      ImVec2 imageMin = {imageRegionMin.x + (regionW - logoW) / 2, imageRegionMin.y + (regionH - logoH) / 2};
      drawList->AddImage(TexRef(g_projectLogo), imageMin, {imageMin.x + logoW, imageMin.y + logoH},
                         {0, 0}, {1, 1}, colWhite);
    }

    SetHorizontalMarqueeFade(marqueeTextMin, marqueeTextMax, Scale(32));
    DrawTextWithMarquee(g_bodyFont, fontSize, marqueeTextPos, marqueeTextMin, marqueeTextMax,
                        colWhite, g_creditsStr.c_str(), g_installerEndTime, 0.9, Scale(200));
    ResetMarqueeFade();
  }

  ImVec2 sideMin = {descriptionMax.x, descriptionMin.y};
  ImVec2 sideMax = {res.x, descriptionMax.y};
  DrawContainer(sideMin, sideMax, false);
  drawList->PopClipRect();

  EButtonIcon backIcon;
  EButtonIcon selectIcon;
  if (hid::IsInputDeviceController()) {
    backIcon = EButtonIcon::B;
    selectIcon = EButtonIcon::A;
  } else if (hid::g_inputDevice == hid::EInputDevice::Keyboard) {
    backIcon = EButtonIcon::Escape;
    selectIcon = EButtonIcon::Enter;
  } else {
    backIcon = EButtonIcon::Escape;
    selectIcon = EButtonIcon::LMB;
  }

  if (g_currentPage == WizardPage::InstallSucceeded && textAlpha >= 1.0) {
    ButtonGuide::Open(Button("Common_Select", 115.0f, selectIcon));
  } else if (g_currentPage != WizardPage::Installing && textAlpha >= 1.0) {
    const char* backKey = "Common_Back";
    if ((g_currentPage == g_firstPage) || (g_currentPage == WizardPage::InstallFailed)) {
      backKey = "Common_Quit";
    }

    std::array<Button, 2> buttons = {Button("Common_Select", 115.0f, selectIcon),
                                     Button(backKey, FLT_MAX, backIcon)};

    ButtonGuide::Open(buttons);
  } else if (g_currentPage == WizardPage::Installing) {
    ButtonGuide::Open(Button("Common_Cancel", FLT_MAX, backIcon));
  } else {
    ButtonGuide::Close();
  }

  ResetProceduralOrigin();
}

static void DrawButtonContainer(ImVec2 min, ImVec2 max, int baser, int baseg, float alpha) {
  // UnleashedRecomp's green buttons in Sonic the Fighters' blue; hovered ones
  // brighten to cyan.
  auto drawList = ImGui::GetBackgroundDrawList();
  SetShaderModifier(IMGUI_SHADER_MODIFIER_SCANLINE_BUTTON);
  int r = baser / 3, g = 90 + baseg * 2, b = 210 + baser / 2;
  drawList->AddRectFilledMultiColor(min, max, IM_COL32(r, g, b, int(223 * alpha)),
                                    IM_COL32(r, g, b, int(178 * alpha)),
                                    IM_COL32(r, g, b, int(223 * alpha)),
                                    IM_COL32(r, g, b, int(178 * alpha)));
  drawList->AddRectFilledMultiColor(min, max, IM_COL32(0, 20, 60, int(13 * alpha)),
                                    IM_COL32(0, 20, 60, 0), IM_COL32(0, 20, 60, int(55 * alpha)),
                                    IM_COL32(0, 20, 60, int(6 * alpha)));
  drawList->AddRectFilledMultiColor(min, max, IM_COL32(r, g + 40, 255, int(13 * alpha)),
                                    IM_COL32(r, g + 40, 255, int(111 * alpha)),
                                    IM_COL32(r, g + 40, 255, 0),
                                    IM_COL32(r, g + 40, 255, int(55 * alpha)));
  SetShaderModifier(IMGUI_SHADER_MODIFIER_NONE);
}

static ImVec2 ComputeTextSize(InstallerFont* font, const char* text, float size, float& squashRatio,
                              float maxTextWidth = FLT_MAX) {
  ImVec2 textSize = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  if (textSize.x > maxTextWidth) {
    squashRatio = maxTextWidth / textSize.x;
  } else {
    squashRatio = 1.0f;
  }

  return textSize;
}

static InstallerFont* ButtonFont(const char* text, bool sourceButton) {
  // The title font only has Latin letters.
  return (sourceButton || HasWideCharacters(text)) ? g_bodyFont : g_titleFont;
}

static void DrawButton(ImVec2 min, ImVec2 max, const char* buttonText, bool sourceButton,
                       bool buttonEnabled, bool& buttonPressed, float maxTextWidth = FLT_MAX,
                       bool makeDefault = false) {
  buttonPressed = false;

  float alpha = float(ComputeMotionInstaller(g_appearTime, g_disappearTime, CONTAINER_INNER_TIME,
                                             CONTAINER_INNER_DURATION));
  if (!buttonEnabled) {
    alpha *= 0.5f;
  }

  int baser = 0;
  int baseg = 0;
  if (g_currentMessagePrompt.empty() && !g_currentPickerVisible && !sourceButton && buttonEnabled &&
      (alpha >= 1.0f)) {
    bool cursorOnButton = PushCursorRect(min, max, buttonPressed, makeDefault);
    if (cursorOnButton) {
      baser = 48;
      baseg = 32;
    }
  }

  DrawButtonContainer(min, max, baser, baseg, alpha);

  InstallerFont* font = ButtonFont(buttonText, sourceButton);
  float size = Scale(sourceButton ? 16.5f : 20.0f);
  float squashRatio;
  ImVec2 textSize = ComputeTextSize(font, buttonText, size, squashRatio, Scale(maxTextWidth));
  min.x += ((max.x - min.x) - textSize.x) / 2.0f;
  min.y += ((max.y - min.y) - textSize.y) / 2.0f;

  if (!sourceButton) {
    // Fixes slight misalignment caused by this particular font.
    min.y -= Scale(1.0f);
  }

  SetOrigin({min.x + textSize.x / 2.0f, min.y});
  SetScale({squashRatio, 1.0f});
  if (baser) {
    // Hovered: the game's yellow.
    SetGradient(min, {min.x + textSize.x, min.y + textSize.y}, IM_COL32(255, 245, 90, 255),
                IM_COL32(255, 180, 0, 255));
  } else {
    SetGradient(min, {min.x + textSize.x, min.y + textSize.y}, IM_COL32(255, 255, 255, 255),
                IM_COL32(170, 215, 255, 255));
  }

  DrawTextWithOutline(font, size, min, IM_COL32(255, 255, 255, int(255 * alpha)), buttonText, 4,
                      IM_COL32(0, 10, 40, int(255 * alpha)));

  ResetGradient();
  SetScale({1.0f, 1.0f});
  SetOrigin({0.0f, 0.0f});
}

enum ButtonColumn { ButtonColumnLeft, ButtonColumnMiddle, ButtonColumnRight };

static void ComputeButtonColumnCoordinates(ButtonColumn buttonColumn, float& minX, float& maxX) {
  switch (buttonColumn) {
    case ButtonColumnLeft:
      minX = g_aspectRatioOffsetX + Scale(CONTAINER_X + CONTAINER_BUTTON_GAP);
      maxX = g_aspectRatioOffsetX + Scale(CONTAINER_X + CONTAINER_BUTTON_GAP + CONTAINER_BUTTON_WIDTH);
      break;
    case ButtonColumnMiddle:
      minX = g_aspectRatioOffsetX + Scale(CONTAINER_X + CONTAINER_WIDTH / 2.0f - CONTAINER_BUTTON_WIDTH / 2.0f);
      maxX = g_aspectRatioOffsetX + Scale(CONTAINER_X + CONTAINER_WIDTH / 2.0f + CONTAINER_BUTTON_WIDTH / 2.0f);
      break;
    case ButtonColumnRight:
      minX = g_aspectRatioOffsetX + Scale(CONTAINER_X + CONTAINER_WIDTH - CONTAINER_BUTTON_GAP - CONTAINER_BUTTON_WIDTH);
      maxX = g_aspectRatioOffsetX + Scale(CONTAINER_X + CONTAINER_WIDTH - CONTAINER_BUTTON_GAP);
      break;
  }
}

static void DrawSourceButton(ButtonColumn buttonColumn, float yRatio, const char* sourceText, bool sourceSet) {
  bool buttonPressed;
  float minX, maxX;
  ComputeButtonColumnCoordinates(buttonColumn, minX, maxX);

  float minusY = (CONTAINER_BUTTON_GAP + BUTTON_HEIGHT) * yRatio;
  ImVec2 min = {minX, g_aspectRatioOffsetY + Scale(CONTAINER_Y + CONTAINER_HEIGHT - CONTAINER_BUTTON_GAP - BUTTON_HEIGHT - minusY)};
  ImVec2 max = {maxX, g_aspectRatioOffsetY + Scale(CONTAINER_Y + CONTAINER_HEIGHT - CONTAINER_BUTTON_GAP - minusY)};

  auto alphaMotion = ComputeMotionInstaller(g_appearTime, g_disappearTime, CONTAINER_INNER_TIME,
                                            CONTAINER_INNER_DURATION);
  auto lightSize = Scale(14);

  DrawButton(min, max, sourceText, true, sourceSet, buttonPressed,
             ((max.x - min.x) * 0.7f) / g_aspectRatioScale);
  DrawToggleLight({min.x + lightSize, min.y + ((max.y - min.y) - lightSize) / 2 + Scale(1)}, sourceSet,
                  float((sourceSet ? 1.0f : 0.5f) * alphaMotion));
}

static void DrawProgressBar(float progressRatio) {
  auto drawList = ImGui::GetBackgroundDrawList();
  float alpha = 1.0;
  const uint32_t innerColor0 = IM_COL32(0, 34, 80, int(255 * alpha));
  const uint32_t innerColor1 = IM_COL32(0, 14, 40, int(255 * alpha));
  float xPadding = Scale(4);
  float yPadding = Scale(3);
  ImVec2 min = {g_aspectRatioOffsetX + Scale(CONTAINER_X) + BOTTOM_X_GAP + Scale(1),
                g_aspectRatioOffsetY + Scale(CONTAINER_Y + CONTAINER_HEIGHT + BOTTOM_Y_GAP)};
  ImVec2 max = {g_aspectRatioOffsetX + Scale(CONTAINER_X + CONTAINER_WIDTH - BOTTOM_X_GAP),
                g_aspectRatioOffsetY + Scale(CONTAINER_Y + CONTAINER_HEIGHT + BOTTOM_Y_GAP + BUTTON_HEIGHT)};

  DrawButtonContainer(min, max, 0, 0, alpha);

  drawList->AddRectFilledMultiColor({min.x + xPadding, min.y + yPadding}, {max.x - xPadding, max.y - yPadding},
                                    innerColor0, innerColor0, innerColor1, innerColor1);

  const uint32_t sliderColor0 = IM_COL32(70, 215, 255, int(255 * alpha));
  const uint32_t sliderColor1 = IM_COL32(0, 90, 210, int(255 * alpha));
  xPadding += Scale(1.5f);
  yPadding += Scale(1.5f);

  ImVec2 sliderMin = {min.x + xPadding, min.y + yPadding};
  ImVec2 sliderMax = {max.x - xPadding, max.y - yPadding};
  sliderMax.x = sliderMin.x + (sliderMax.x - sliderMin.x) * progressRatio;
  drawList->AddRectFilledMultiColor(sliderMin, sliderMax, sliderColor0, sliderColor0, sliderColor1, sliderColor1);
}

static void PickerThreadProcess() {
  // The Windows file dialog (UnleashedRecomp: nativefiledialog).
  HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  IFileOpenDialog* dialog = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog)))) {
    DWORD options = 0;
    dialog->GetOptions(&options);
    options |= FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (g_currentPickerFolderMode) options |= FOS_PICKFOLDERS;
    dialog->SetOptions(options);
    HRESULT shown = dialog->Show(g_pickerOwner);
    IShellItemArray* items = nullptr;
    if (SUCCEEDED(shown) && SUCCEEDED(dialog->GetResults(&items))) {
      DWORD count = 0;
      items->GetCount(&count);
      for (DWORD i = 0; i < count; ++i) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &item))) {
          PWSTR path = nullptr;
          if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            g_currentPickerResults.emplace_back(path);
            CoTaskMemFree(path);
          }
          item->Release();
        }
      }
      items->Release();
    } else if (FAILED(shown) && shown != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
      g_currentPickerErrorMessage = "The file dialog could not be opened.";
    }
    dialog->Release();
  } else {
    g_currentPickerErrorMessage = "The file dialog could not be opened.";
  }
  if (SUCCEEDED(init)) CoUninitialize();

  g_currentPickerResultsReady = true;
}

static void PickerStart(bool folderMode) {
  if (g_currentPickerThread != nullptr) {
    g_currentPickerThread->join();
    g_currentPickerThread.reset();
  }

  g_currentPickerResults.clear();
  g_currentPickerFolderMode = folderMode;
  g_currentPickerResultsReady = false;
  g_currentPickerVisible = true;
  g_pickerOwner = GetForegroundWindow();

  g_currentPickerThread = std::make_unique<std::thread>(PickerThreadProcess);
}

static void PickerShow(bool folderMode) {
  if (g_pickerTutorialCleared[folderMode]) {
    PickerStart(folderMode);
  } else {
    g_currentMessagePrompt = Localise(folderMode ? "Installer_Message_FolderPickerTutorial"
                                                 : "Installer_Message_FilePickerTutorial");
    g_currentMessagePromptConfirmation = false;
    g_pickerTutorialTriggered = true;
    g_pickerTutorialFolderMode = folderMode;
  }
}

static bool ParseSourcePaths(std::list<std::filesystem::path>& paths) {
  constexpr size_t failedPathLimit = 5;
  bool isFailedPathsOverLimit = false;
  std::list<std::filesystem::path> failedPaths;
  for (const std::filesystem::path& path : paths) {
    std::filesystem::path package;
    if (Installer::parseGame(path, package)) {
      g_gameSourcePath = package;
    } else if (failedPaths.size() < failedPathLimit) {
      failedPaths.push_back(path);
    } else {
      isFailedPathsOverLimit = true;
    }
  }

  if (!failedPaths.empty()) {
    std::stringstream stringStream;
    stringStream << Localise("Installer_Message_InvalidFilesList") << std::endl;
    for (const std::filesystem::path& path : failedPaths) {
      std::u8string filenameU8 = path.filename().u8string();
      stringStream << std::endl
                   << "- " << Truncate(std::string(filenameU8.begin(), filenameU8.end()), 32, true, true);
    }

    if (isFailedPathsOverLimit) stringStream << std::endl << "- [...]";

    g_currentMessagePrompt = stringStream.str();
    g_currentMessagePromptConfirmation = false;
  }

  return failedPaths.empty();
}

static void DrawLanguagePicker() {
  if (g_currentPage == WizardPage::SelectLanguage) {
    float alphaMotion = float(ComputeMotionInstaller(g_appearTime, g_disappearTime,
                                                     CONTAINER_INNER_TIME, CONTAINER_INNER_DURATION));
    float minX, maxX;
    bool buttonPressed;

    for (int i = 0; i < 6; i++) {
      ComputeButtonColumnCoordinates((i < 3) ? ButtonColumnLeft : ButtonColumnRight, minX, maxX);

      float minusY = (CONTAINER_BUTTON_GAP + BUTTON_HEIGHT) * (float(i % 3));
      ImVec2 min = {minX, g_aspectRatioOffsetY + Scale(CONTAINER_Y + CONTAINER_HEIGHT - CONTAINER_BUTTON_GAP - BUTTON_HEIGHT - minusY)};
      ImVec2 max = {maxX, g_aspectRatioOffsetY + Scale(CONTAINER_Y + CONTAINER_HEIGHT - CONTAINER_BUTTON_GAP - minusY)};

      auto lightSize = Scale(14);

      DrawButton(min, max, LANGUAGE_TEXT[i], false, true, buttonPressed, FLT_MAX,
                 LANGUAGE_ENUM[i] == ELanguage::English);
      DrawToggleLight({min.x + lightSize, min.y + ((max.y - min.y) - lightSize) / 2 + Scale(1)},
                      Config::Language == LANGUAGE_ENUM[i], alphaMotion);

      if (buttonPressed) Config::Language = LANGUAGE_ENUM[i];
    }
  }
}

static void DrawSourcePickers() {
  bool buttonPressed = false;
  if (g_currentPage == WizardPage::SelectGame) {
    constexpr float ADD_BUTTON_MAX_TEXT_WIDTH = 168.0f;
    const std::string& addFilesText = Localise("Installer_Button_AddFiles");
    float squashRatio;
    ImVec2 textSize = ComputeTextSize(ButtonFont(addFilesText.c_str(), false), addFilesText.c_str(),
                                      20.0f, squashRatio, ADD_BUTTON_MAX_TEXT_WIDTH);
    ImVec2 min = {g_aspectRatioOffsetX + Scale(CONTAINER_X + BOTTOM_X_GAP),
                  g_aspectRatioOffsetY + Scale(CONTAINER_Y + CONTAINER_HEIGHT + BOTTOM_Y_GAP)};
    ImVec2 max = {g_aspectRatioOffsetX + Scale(CONTAINER_X + BOTTOM_X_GAP + textSize.x * squashRatio + BUTTON_TEXT_GAP),
                  g_aspectRatioOffsetY + Scale(CONTAINER_Y + CONTAINER_HEIGHT + BOTTOM_Y_GAP + BUTTON_HEIGHT)};
    DrawButton(min, max, addFilesText.c_str(), false, true, buttonPressed, ADD_BUTTON_MAX_TEXT_WIDTH);
    if (buttonPressed) {
      PickerShow(false);
    }

    min.x += Scale(BOTTOM_X_GAP + textSize.x * squashRatio + BUTTON_TEXT_GAP);

    const std::string& addFolderText = Localise("Installer_Button_AddFolder");
    textSize = ComputeTextSize(ButtonFont(addFolderText.c_str(), false), addFolderText.c_str(), 20.0f,
                               squashRatio, ADD_BUTTON_MAX_TEXT_WIDTH);
    max.x = min.x + Scale(textSize.x * squashRatio + BUTTON_TEXT_GAP);
    DrawButton(min, max, addFolderText.c_str(), false, true, buttonPressed, ADD_BUTTON_MAX_TEXT_WIDTH);
    if (buttonPressed) {
      PickerShow(true);
    }
  }
}

static void DrawSources() {
  if (g_currentPage == WizardPage::SelectGame) {
    DrawSourceButton(ButtonColumnMiddle, 0, Localise("Installer_Step_Game").c_str(), !g_gameSourcePath.empty());
  }
}

static void DrawInstallingProgress() {
  if (g_currentPage == WizardPage::Installing) {
    constexpr float ProgressSpeed = 0.1f;
    float ratioTarget = g_installerProgressRatioTarget.load();
    g_installerProgressRatioCurrent += std::min(ratioTarget - g_installerProgressRatioCurrent,
                                                ProgressSpeed * ImGui::GetIO().DeltaTime);
    DrawProgressBar(g_installerProgressRatioCurrent);

    if (g_installerFinished) {
      g_installerThread->join();
      g_installerThread.reset();
      g_installerEndTime = ImGui::GetTime();
      g_currentPage = g_installerFailed ? WizardPage::InstallFailed : WizardPage::InstallSucceeded;
      Game_PlaySound(g_installerFailed ? InstallerSound::Cancel : InstallerSound::Ring);
    }
  }
}

static void InstallerThread() {
  if (!Installer::install(g_installerSources, g_installPath, g_installerJournal, [&]() {
        g_installerProgressRatioTarget = float(double(g_installerJournal.progressCounter) /
                                               double(g_installerJournal.progressTotal));

        // If user is being asked for confirmation on cancelling the installation, halt the installer from progressing further.
        g_installerHalted.wait(true);

        // If user has confirmed they wish to cancel the installation, return false to indicate the installer should fail and stop.
        return !g_installerCancelled.load();
      })) {
    g_installerFailed = true;
    g_installerErrorMessage = g_installerJournal.lastErrorMessage;

    // Delete all files that were copied.
    Installer::rollback(g_installerJournal);
  }

  g_installerFinished = true;
  g_installerCancelled = false;
}

static void InstallerStart() {
  g_currentPage = WizardPage::Installing;
  g_installerStartTime = ImGui::GetTime();
  g_installerEndTime = DBL_MAX;
  g_installerProgressRatioCurrent = 0.0f;
  g_installerProgressRatioTarget = 0.0f;
  g_installerFailed = false;
  g_installerFinished = false;
  g_installerJournal = Journal();
  g_installerThread = std::make_unique<std::thread>(InstallerThread);
}

static bool InstallerParseSources(std::string& errorMessage) {
  std::error_code spaceErrorCode;
  std::filesystem::path spacePath = g_installPath;
  while (!spacePath.empty() && !std::filesystem::exists(spacePath, spaceErrorCode)) {
    spacePath = spacePath.parent_path();
  }
  std::filesystem::space_info spaceInfo = std::filesystem::space(spacePath, spaceErrorCode);
  if (!spaceErrorCode) {
    g_installerAvailableSize = spaceInfo.available;
  }

  bool sourcesParsed = Installer::parseSources(g_gameSourcePath, g_installerJournal, g_installerSources);
  errorMessage = g_installerJournal.lastErrorMessage;
  return sourcesParsed;
}

static void DrawNavigationButton() {
  if (g_currentPage == WizardPage::Installing) {
    // Navigation buttons are not offered during installation at the moment.
    return;
  }

  bool nextButtonEnabled = !g_isDisappearing && (g_currentPage != WizardPage::Installing);
  if (nextButtonEnabled && g_currentPage == WizardPage::SelectGame) {
    nextButtonEnabled = !g_gameSourcePath.empty();
  }
  if (nextButtonEnabled && g_currentPage == WizardPage::CheckSpace) {
    nextButtonEnabled = g_installerAvailableSize == 0 || g_installerAvailableSize > g_installerSources.totalSize;
  }

  float squashRatio;
  constexpr float NAV_BUTTON_MAX_TEXT_WIDTH = 90.0f;
  std::string_view nextButtonKey = "Installer_Button_Next";
  if (g_currentPage == WizardPage::InstallFailed) {
    nextButtonKey = "Installer_Button_Retry";
  }

  const std::string& nextButtonText = Localise(nextButtonKey);
  ImVec2 nextTextSize = ComputeTextSize(ButtonFont(nextButtonText.c_str(), true), nextButtonText.c_str(),
                                        20.0f, squashRatio, NAV_BUTTON_MAX_TEXT_WIDTH);
  ImVec2 min = {g_aspectRatioOffsetX + Scale(CONTAINER_X + CONTAINER_WIDTH - nextTextSize.x * squashRatio - BOTTOM_X_GAP - BUTTON_TEXT_GAP),
                g_aspectRatioOffsetY + Scale(CONTAINER_Y + CONTAINER_HEIGHT + BOTTOM_Y_GAP)};
  ImVec2 max = {g_aspectRatioOffsetX + Scale(CONTAINER_X + CONTAINER_WIDTH - BOTTOM_X_GAP),
                g_aspectRatioOffsetY + Scale(CONTAINER_Y + CONTAINER_HEIGHT + BOTTOM_Y_GAP + BUTTON_HEIGHT)};

  bool buttonPressed = false;
  DrawButton(min, max, nextButtonText.c_str(), false, nextButtonEnabled, buttonPressed, NAV_BUTTON_MAX_TEXT_WIDTH);

  if (buttonPressed) {
    if (g_currentPage == WizardPage::SelectGame) {
      std::string sourcesErrorMessage;
      if (!InstallerParseSources(sourcesErrorMessage)) {
        // The package isn't the supported one.
        g_currentMessagePrompt = Localise("Installer_Message_UnsupportedGame");
        g_currentMessagePromptConfirmation = false;
        g_gameSourcePath.clear();
      } else {
        g_currentPage = WizardPage::CheckSpace;
      }
    } else if (g_currentPage == WizardPage::CheckSpace) {
      InstallerStart();
    } else if (g_currentPage == WizardPage::InstallSucceeded) {
      g_isDisappearing = true;
      g_disappearTime = ImGui::GetTime();
    } else if (g_currentPage == WizardPage::InstallFailed) {
      g_currentPage = g_firstPage;
    } else {
      g_currentPage = WizardPage(int(g_currentPage) + 1);
    }
  }
}

static void CheckCancelAction() {
  if (!g_currentCursorBack) {
    return;
  }

  g_currentCursorBack = false;

  if (g_currentPage == WizardPage::InstallSucceeded) {
    // Nothing to back out on this page.
    return;
  }
  if (g_currentPage == WizardPage::Installing && g_installerCancelled) {
    // Installer's already been cancelled, no need for more confirmations.
    return;
  }

  Game_PlaySound(InstallerSound::Cancel);

  if (g_currentPage == g_firstPage || g_currentPage == WizardPage::InstallFailed) {
    // Ask for confirmation if user wants to quit the installer.
    g_currentMessagePrompt = Localise("Installer_Message_Quit");
    g_currentMessagePromptSource = MessagePromptSource::Back;
    g_currentMessagePromptConfirmation = true;
  } else if (g_currentPage == WizardPage::Installing) {
    // Ask for confirmation if the user wants to cancel the installation.
    g_currentMessagePrompt = Localise("Installer_Message_Cancel");
    g_currentMessagePromptSource = MessagePromptSource::Back;
    g_currentMessagePromptConfirmation = true;

    // Indicate to the installer that all progress should stop until the user confirms if they wish to cancel.
    g_installerHalted = true;
  } else if (int(g_currentPage) > 0) {
    // Just go back to the previous page.
    g_currentPage = WizardPage(int(g_currentPage) - 1);
  }
}

static void DrawHorizontalBorder(bool bottomBorder) {
  const uint32_t FADE_COLOR_LEFT = IM_COL32(155, 175, 215, 0);
  const uint32_t SOLID_COLOR = IM_COL32(170, 205, 255, 255);
  const uint32_t FADE_COLOR_RIGHT = IM_COL32(170, 225, 255, 0);
  auto drawList = ImGui::GetBackgroundDrawList();
  double borderScale = 1.0 - ComputeMotionInstaller(g_appearTime, g_disappearTime, CONTAINER_LINE_ANIMATION_TIME,
                                                    CONTAINER_LINE_ANIMATION_DURATION);
  float midX = g_aspectRatioOffsetX + Scale(CONTAINER_X + CONTAINER_WIDTH / 5);
  float minX = std::lerp(g_aspectRatioOffsetX + Scale(CONTAINER_X - BORDER_SIZE - BORDER_OVERSHOOT), midX, float(borderScale));
  float maxX = std::lerp(g_aspectRatioOffsetX + Scale(CONTAINER_X + CONTAINER_WIDTH + SIDE_CONTAINER_WIDTH + BORDER_OVERSHOOT),
                         midX, float(borderScale));
  float minY = g_aspectRatioOffsetY + (bottomBorder ? Scale(CONTAINER_Y + CONTAINER_HEIGHT) : Scale(CONTAINER_Y - BORDER_SIZE));
  float maxY = minY + Scale(BORDER_SIZE);
  drawList->AddRectFilledMultiColor({minX, minY}, {midX, maxY}, FADE_COLOR_LEFT, SOLID_COLOR, SOLID_COLOR,
                                    FADE_COLOR_LEFT);

  drawList->AddRectFilledMultiColor({midX, minY}, {maxX, maxY}, SOLID_COLOR, FADE_COLOR_RIGHT, FADE_COLOR_RIGHT,
                                    SOLID_COLOR);
}

static void DrawVerticalBorder(bool rightBorder) {
  const uint32_t SOLID_COLOR = IM_COL32(170, rightBorder ? 225 : 205, 255, 255);
  const uint32_t FADE_COLOR = IM_COL32(170, rightBorder ? 225 : 205, 255, 0);
  auto drawList = ImGui::GetBackgroundDrawList();
  double borderScale = 1.0 - ComputeMotionInstaller(g_appearTime, g_disappearTime, CONTAINER_LINE_ANIMATION_TIME,
                                                    CONTAINER_LINE_ANIMATION_DURATION);
  float minX = g_aspectRatioOffsetX + (rightBorder ? Scale(CONTAINER_X + CONTAINER_WIDTH) : Scale(CONTAINER_X - BORDER_SIZE));
  float maxX = minX + Scale(BORDER_SIZE);
  float midY = g_aspectRatioOffsetY + Scale(CONTAINER_Y + CONTAINER_HEIGHT / 2);
  float minY = std::lerp(g_aspectRatioOffsetY + Scale(CONTAINER_Y - BORDER_OVERSHOOT), midY, float(borderScale));
  float maxY = std::lerp(g_aspectRatioOffsetY + Scale(CONTAINER_Y + CONTAINER_HEIGHT + BORDER_OVERSHOOT), midY,
                         float(borderScale));
  drawList->AddRectFilledMultiColor({minX, minY}, {maxX, midY}, FADE_COLOR, FADE_COLOR, SOLID_COLOR, SOLID_COLOR);

  drawList->AddRectFilledMultiColor({minX, midY}, {maxX, maxY}, SOLID_COLOR, SOLID_COLOR, FADE_COLOR, FADE_COLOR);
}

static void DrawBorders() {
  DrawHorizontalBorder(false);
  DrawHorizontalBorder(true);
  DrawVerticalBorder(false);
  DrawVerticalBorder(true);
}

static void DrawMessagePrompt() {
  if (g_currentMessagePrompt.empty()) {
    return;
  }

  bool messageWindowReturned = false;
  if (g_currentMessagePromptConfirmation) {
    std::array<std::string, 2> YesNoButtons = {Localise("Common_Yes"), Localise("Common_No")};
    messageWindowReturned = MessageWindow::Open(g_currentMessagePrompt, &g_currentMessageResult, YesNoButtons, 1);
  } else {
    messageWindowReturned = MessageWindow::Open(g_currentMessagePrompt, &g_currentMessageResult);
  }

  if (messageWindowReturned) {
    if (g_currentMessagePromptConfirmation && (g_currentMessageResult == 0)) {
      if (g_currentMessagePromptSource == MessagePromptSource::Back) {
        if (g_currentPage == WizardPage::Installing) {
          // If user confirms they wish to cancel the installation, notify the installation thread it must finish as soon as possible.
          g_installerCancelled = true;
        } else {
          // In all cases, proceed to just quit the application.
          g_isQuitting = true;
          g_isDisappearing = true;
          g_disappearTime = ImGui::GetTime();
        }
      }
    }

    if (g_currentMessagePromptSource == MessagePromptSource::Back) {
      // Regardless of the confirmation, the installation thread must be resumed.
      g_installerHalted = false;
      g_installerHalted.notify_all();
    }

    g_currentMessagePrompt.clear();
    g_currentMessagePromptSource = MessagePromptSource::Unknown;
    g_currentMessageResult = -1;
  }
}

static void PickerDrawForeground() {
  if (g_currentPickerVisible) {
    auto drawList = ImGui::GetBackgroundDrawList();
    drawList->AddRectFilled({0.0f, 0.0f}, ImGui::GetIO().DisplaySize, IM_COL32(0, 0, 0, 190));
  }
}

static void PickerCheckTutorial() {
  if (!g_pickerTutorialTriggered || !g_currentMessagePrompt.empty()) {
    return;
  }

  PickerStart(g_pickerTutorialFolderMode);
  g_pickerTutorialTriggered = false;
}

static void PickerCheckResults() {
  if (!g_currentPickerResultsReady) {
    return;
  }

  if (g_currentPickerThread) {
    g_currentPickerThread->join();
    g_currentPickerThread.reset();
  }

  if (!g_currentPickerErrorMessage.empty()) {
    g_currentMessagePrompt = g_currentPickerErrorMessage;
    g_currentMessagePromptConfirmation = false;
    g_currentPickerErrorMessage.clear();
  }

  if (!g_currentPickerResults.empty() && ParseSourcePaths(g_currentPickerResults)) {
    g_pickerTutorialCleared[g_pickerTutorialFolderMode] = true;
  }

  g_currentPickerResultsReady = false;
  g_currentPickerVisible = false;
}

static void ProcessMusic() {
  if (g_isDisappearing) {
    InstallerAudio::FadeOutMusic();
  } else {
    InstallerAudio::PlayMusic();
  }
}

bool InstallerWizard::Init(rex::ui::ImmediateDrawer& drawer, std::filesystem::path installPath) {
  if (!InstallerAssets::Load(drawer)) {
    return false;
  }
  g_bodyFont = InstallerAssets::BodyFont();
  g_titleFont = InstallerAssets::TitleFont();
  for (size_t i = 0; i < g_installTextures.size(); ++i) {
    g_installTextures[i] = InstallerAssets::Texture("character_" + std::to_string(i));
  }
  g_sonicIcon = InstallerAssets::Texture("sonic_icon");
  g_loadingArc = InstallerAssets::Texture("loading_arc");
  g_ringGlow = InstallerAssets::Texture("ring_glow");
  g_projectLogo = InstallerAssets::Texture("project_logo");

  g_creditsStr = Localise("Credits");
  for (char& c : g_creditsStr) {
    if (c == ',') c = ' ';
  }

  g_installPath = std::move(installPath);
  InstallerAudio::Init();
  InstallerInput::Init();

  g_currentPage = g_firstPage;
  g_appearTime = ImGui::GetTime();
  g_disappearTime = DBL_MAX;
  g_isDisappearing = false;
  g_isQuitting = false;
  s_isVisible = true;
  return true;
}

void InstallerWizard::Draw() {
  if (!s_isVisible) {
    return;
  }

  UpdateAspectRatio(ImGui::GetIO().DisplaySize);
  for (const InstallerInputEvent& event : InstallerInput::Poll()) {
    if (MessageWindow::s_isVisible) {
      MessageWindow::HandleInput(event);
    } else {
      HandleInput(event);
    }
  }

  ProcessMusic();
  ResetCursorRects();
  DrawBackground();
  DrawLeftImage();
  DrawScanlineBars();
  DrawDescriptionContainer();
  DrawLanguagePicker();
  DrawSourcePickers();
  DrawSources();
  DrawInstallingProgress();
  DrawNavigationButton();
  CheckCancelAction();
  DrawBorders();
  DrawMessagePrompt();
  PickerDrawForeground();
  PickerCheckTutorial();
  PickerCheckResults();
  MessageWindow::Draw();
  ButtonGuide::Draw();

  if (g_isDisappearing) {
    double disappearDuration = ALL_ANIMATIONS_FULL_DURATION / 60.0;
    if (g_isQuitting) {
      // Add some extra waiting time when quitting the application altogether.
      disappearDuration += QUITTING_EXTRA_DURATION / 60.0;
    }

    if (ImGui::GetTime() > (g_disappearTime + disappearDuration)) {
      s_isVisible = false;
    }
  }
}

bool InstallerWizard::IsQuitting() {
  return g_isQuitting;
}

bool InstallerWizard::OnCloseRequested() {
  if (!s_isVisible || g_currentPage != WizardPage::Installing) {
    return true;
  }
  HandleInput({InstallerInputEvent::Type::Quit});
  return false;
}

void InstallerWizard::Shutdown() {
  // Wait for and erase the threads.
  if (g_installerThread != nullptr) {
    g_installerCancelled = true;
    g_installerHalted = false;
    g_installerHalted.notify_all();
    g_installerThread->join();
    g_installerThread.reset();
  }

  if (g_currentPickerThread != nullptr) {
    g_currentPickerThread->join();
    g_currentPickerThread.reset();
  }

  ButtonGuide::Close();
  InstallerInput::Shutdown();
  InstallerAudio::Shutdown();
  InstallerAssets::Unload();
  s_isVisible = false;
}
