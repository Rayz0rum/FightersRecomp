// Ported from UnleashedRecomp's ui/installer_wizard.cpp (hedge-dev, GPL-3.0).
// Changes: drawn like Sonic the Fighters' menus (the page's window of choices
// over the white card, with the game's characters, and the description panel
// under it); its pages (the Xbox 360 World package, no title update or DLC),
// fonts and sounds; input from InstallerInput; the Windows file dialog; drawn a
// frame at a time by the app's dialog.

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

// Layout of Sonic the Fighters' menus at 1280x720, measured from the game.
constexpr float CARD_X0 = 245.0f;
constexpr float CARD_Y0 = 54.0f;
constexpr float CARD_X1 = 1035.0f;
constexpr float CARD_Y1 = 665.0f;
constexpr float CHARACTER_SIZE = 600.0f;
constexpr float CHARACTER_Y = 60.0f;

constexpr float WINDOW_X0 = 255.0f;
constexpr float WINDOW_X1 = 1026.0f;
constexpr float WINDOW_Y0 = 85.0f;
// The lowest the window goes, above the description panel.
constexpr float WINDOW_MAX_Y1 = 488.0f;
constexpr float TEXT_MARGIN_X = 40.0f;
constexpr float ROW_HEIGHT = 36.0f;

constexpr float DESCRIPTION_X0 = 97.0f;
constexpr float DESCRIPTION_Y0 = 511.0f;
constexpr float DESCRIPTION_X1 = 1192.0f;
constexpr float DESCRIPTION_Y1 = 615.0f;

constexpr float BAND_Y = 624.0f;

// The game's fonts at their 1920x1080 size, and its line spacing.
constexpr float BODY_FONT_SIZE = 46.0f * 2.0f / 3.0f;
constexpr float TITLE_FONT_SIZE = 56.0f * 2.0f / 3.0f;
constexpr float LINE_HEIGHT = 36.0f;

// Fades, in seconds.
constexpr double APPEAR_DURATION = 0.3;
constexpr double DISAPPEAR_DURATION = 0.5;
constexpr double QUITTING_EXTRA_DURATION = 0.5;


static InstallerFont* g_bodyFont;
static InstallerFont* g_titleFont;

static double g_appearTime = 0.0;
static double g_disappearTime = DBL_MAX;
static bool g_isDisappearing = false;
static bool g_isQuitting = false;

static std::filesystem::path g_installPath;
static std::filesystem::path g_gameSourcePath;
static std::array<GuestTexture*, 10> g_installTextures;
static GuestTexture* g_loadingArc;
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

      // The selection stays when the mouse leaves the choices, like in the game's menus.

      break;
    }

    default:
      break;
  }

  // The choices are a list, like the game's menus: up and down go to the
  // previous and next one, round from the ends.
  int count = int(g_currentCursorRects.size());
  if (tapDirection.y != 0.0f && count > 0) {
    if (g_currentCursorIndex >= count || g_currentCursorIndex < 0) {
      newCursorIndex = g_currentCursorDefault;
    } else {
      newCursorIndex = (g_currentCursorIndex + (tapDirection.y > 0.0f ? 1 : count - 1)) % count;
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

// The languages, in the order of the list.
const char* LANGUAGE_TEXT[] = {
    "English", "日本語", "Deutsch", "Français", "Español", "Italiano",
};

const ELanguage LANGUAGE_ENUM[] = {
    ELanguage::English, ELanguage::Japanese, ELanguage::German,
    ELanguage::French,  ELanguage::Spanish,  ELanguage::Italian,
};

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

static float UiAlpha() {
  double time = ImGui::GetTime();
  double alpha = std::clamp((time - g_appearTime) / APPEAR_DURATION, 0.0, 1.0);
  if (g_isDisappearing) {
    alpha *= 1.0 - std::clamp((time - g_disappearTime) / DISAPPEAR_DURATION, 0.0, 1.0);
  }
  return float(alpha);
}

// A point of the 1280x720 layout on the display.
static ImVec2 LayoutPos(float x, float y) {
  return {g_aspectRatioOffsetX + Scale(x), g_aspectRatioOffsetY + Scale(y)};
}

static void DrawBackground() {
  // The deep blue of the game's menus.
  auto& res = ImGui::GetIO().DisplaySize;
  auto drawList = ImGui::GetBackgroundDrawList();
  drawList->AddRectFilledMultiColor({0.0f, 0.0f}, res, IM_COL32(7, 24, 90, 255),
                                    IM_COL32(7, 24, 90, 255), IM_COL32(1, 9, 40, 255),
                                    IM_COL32(1, 9, 40, 255));
}

static int CharacterIndex() {
  int index = WIZARD_INSTALL_TEXTURE_INDEX[int(g_currentPage)];
  if (g_currentPage == WizardPage::Installing) {
    // Go through them all while installing.
    index += int((ImGui::GetTime() - g_installerStartTime) / 15.0);
  }
  return index % int(g_installTextures.size());
}

static void DrawCard() {
  // The white card of the game's menus, with the page's character standing
  // on it where the menus have the logo.
  float alpha = UiAlpha();
  auto drawList = ImGui::GetBackgroundDrawList();
  drawList->AddRectFilled(LayoutPos(CARD_X0, CARD_Y0), LayoutPos(CARD_X1, CARD_Y1),
                          IM_COL32(255, 255, 255, int(255 * alpha)));

  // Cross-fade between characters.
  static int shown = -1, previous = -1;
  static double changeTime = 0.0;
  int index = CharacterIndex();
  if (index != shown) {
    previous = shown;
    shown = index;
    changeTime = ImGui::GetTime();
  }
  float fade = float(std::clamp((ImGui::GetTime() - changeTime) / 0.25, 0.0, 1.0));
  constexpr float size = CHARACTER_SIZE;
  float centreX = (CARD_X0 + CARD_X1) / 2;
  auto draw = [&](int i, float a) {
    GuestTexture* texture = i >= 0 ? g_installTextures[i] : nullptr;
    if (!texture || a <= 0.0f) return;
    drawList->AddImage(TexRef(texture), LayoutPos(centreX - size / 2, CHARACTER_Y),
                       LayoutPos(centreX + size / 2, CHARACTER_Y + size), {0, 0}, {1, 1},
                       IM_COL32(255, 255, 255, int(255 * a)));
  };
  drawList->PushClipRect(LayoutPos(CARD_X0, CARD_Y0), LayoutPos(CARD_X1, CARD_Y1), true);
  draw(previous, alpha * (1.0f - fade));
  draw(shown, alpha * fade);
  drawList->PopClipRect();
}

static void DrawBottomBand() {
  // The line across the bottom of the game's menus and the translucent band
  // under it, where the button guide is.
  auto& res = ImGui::GetIO().DisplaySize;
  auto drawList = ImGui::GetBackgroundDrawList();
  float u = Scale(1.0f);
  float y = LayoutPos(0, BAND_Y).y;
  drawList->AddRectFilled({0.0f, y}, {res.x, y + 2 * u}, IM_COL32(39, 40, 41, 255));
  drawList->AddRectFilled({0.0f, y + 2 * u}, {res.x, y + 3 * u}, IM_COL32(206, 216, 239, 255));
  drawList->AddRectFilled({0.0f, y + 3 * u}, {res.x, y + 4 * u}, IM_COL32(157, 169, 195, 255));
  drawList->AddRectFilled({0.0f, y + 4 * u}, res, IM_COL32(2, 18, 50, 207));
  drawList->AddRectFilledMultiColor({0.0f, y + 4 * u}, {res.x, y + 13 * u},
                                    IM_COL32(140, 155, 185, 110), IM_COL32(140, 155, 185, 110),
                                    IM_COL32(140, 155, 185, 0), IM_COL32(140, 155, 185, 0));

  auto fontSize = Scale(16.0f);
  auto textSize = g_bodyFont->CalcTextSizeA(fontSize, FLT_MAX, 0, g_versionString);
  ImVec2 pos = LayoutPos(24.0f, 720.0f - 12.0f);
  g_bodyFont->AddText(drawList, fontSize, {pos.x, pos.y - textSize.y},
                      IM_COL32(255, 255, 255, int(80 * UiAlpha())), g_versionString);
}

enum class RowAction { Language, Next, AddFiles, AddFolder };

struct Row {
  std::string label;
  const std::string* description;
  bool enabled = true;
  RowAction action;
  ELanguage language = ELanguage::English;
};

static bool IsNextEnabled() {
  if (g_isDisappearing) return false;
  if (g_currentPage == WizardPage::SelectGame) return !g_gameSourcePath.empty();
  if (g_currentPage == WizardPage::CheckSpace) {
    return g_installerAvailableSize == 0 || g_installerAvailableSize > g_installerSources.totalSize;
  }
  return true;
}

static std::vector<Row> PageRows() {
  std::vector<Row> rows;
  auto next = [&](const char* label, const char* description) {
    rows.push_back({Localise(label), &Localise(description), IsNextEnabled(), RowAction::Next});
  };
  switch (g_currentPage) {
    case WizardPage::SelectLanguage:
      for (size_t i = 0; i < std::size(LANGUAGE_TEXT); ++i) {
        rows.push_back({LANGUAGE_TEXT[i], &Localise("Installer_Page_SelectLanguage", LANGUAGE_ENUM[i]),
                        !g_isDisappearing, RowAction::Language, LANGUAGE_ENUM[i]});
      }
      break;
    case WizardPage::Introduction: next("Installer_Button_Next", "Installer_Desc_Next"); break;
    case WizardPage::SelectGame:
      rows.push_back({Localise("Installer_Button_AddFiles"), &Localise("Installer_Desc_AddFiles"),
                      !g_isDisappearing, RowAction::AddFiles});
      rows.push_back({Localise("Installer_Button_AddFolder"), &Localise("Installer_Desc_AddFolder"),
                      !g_isDisappearing, RowAction::AddFolder});
      next("Installer_Button_Next", "Installer_Desc_Next");
      break;
    case WizardPage::CheckSpace: next("Installer_Button_Next", "Installer_Desc_Install"); break;
    case WizardPage::Installing: break;
    case WizardPage::InstallSucceeded: next("Installer_Button_Next", "Installer_Desc_Play"); break;
    case WizardPage::InstallFailed: next("Installer_Button_Retry", "Installer_Desc_Retry"); break;
  }
  return rows;
}

static std::string WindowText() {
  std::string text = g_currentPage == WizardPage::SelectLanguage ? "" : GetWizardText(g_currentPage);
  if (g_currentPage == WizardPage::CheckSpace) {
    constexpr double DivisorGiB = (1024.0 * 1024.0 * 1024.0);
    char requiredSpaceText[128];
    char availableSpaceText[128] = "";
    snprintf(requiredSpaceText, sizeof(requiredSpaceText),
             Localise("Installer_Step_RequiredSpace").c_str(),
             double(g_installerSources.totalSize) / DivisorGiB);
    if (g_installerAvailableSize > 0) {
      snprintf(availableSpaceText, sizeof(availableSpaceText),
               Localise("Installer_Step_AvailableSpace").c_str(),
               double(g_installerAvailableSize) / DivisorGiB);
    }
    text += std::string(requiredSpaceText) + "\n" + availableSpaceText;
  } else if (g_currentPage == WizardPage::InstallFailed) {
    // Japanese needs text to be brought in by a normal width space as it
    // allows for text to begin further than others for special characters.
    if (Config::Language == ELanguage::Japanese) text += " ";
    text += g_installerErrorMessage;
  }
  // Blank lines at the end only pad UnleashedRecomp's layout.
  while (!text.empty() && text.back() == '\n') text.pop_back();
  return text;
}

static float LineMargin() {
  return LINE_HEIGHT - BODY_FONT_SIZE + (Config::Language == ELanguage::Japanese ? 1.0f : 0.0f);
}

static void DoRowAction(const Row& row) {
  switch (row.action) {
    case RowAction::Language:
      Config::Language = row.language;
      g_currentPage = WizardPage(int(g_currentPage) + 1);
      break;
    case RowAction::AddFiles: PickerShow(false); break;
    case RowAction::AddFolder: PickerShow(true); break;
    case RowAction::Next:
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
      break;
  }
}

static void UpdateInstallingProgress() {
  if (g_currentPage != WizardPage::Installing) return;
  constexpr float ProgressSpeed = 0.1f;
  float ratioTarget = g_installerProgressRatioTarget.load();
  g_installerProgressRatioCurrent += std::min(ratioTarget - g_installerProgressRatioCurrent,
                                              ProgressSpeed * ImGui::GetIO().DeltaTime);
  if (g_installerFinished) {
    g_installerThread->join();
    g_installerThread.reset();
    g_installerEndTime = ImGui::GetTime();
    g_currentPage = g_installerFailed ? WizardPage::InstallFailed : WizardPage::InstallSucceeded;
    Game_PlaySound(g_installerFailed ? InstallerSound::Cancel : InstallerSound::Ring);
  }
}

static void DrawProgressBar(ImVec2 min, ImVec2 max, float ratio, float alpha) {
  // A sunken track filled like the menus' selection bar.
  auto drawList = ImGui::GetBackgroundDrawList();
  auto a = [&](float v) { return int(std::clamp(v * alpha, 0.0f, 255.0f)); };
  float u = Scale(1.0f);
  drawList->AddRectFilled(min, max, IM_COL32(0, 6, 22, a(200)));
  drawList->AddRect(min, max, IM_COL32(150, 165, 200, a(150)), 0.0f, 0, u);
  ImVec2 fillMin = {min.x + 3 * u, min.y + 3 * u};
  ImVec2 fillMax = {fillMin.x + (max.x - min.x - 6 * u) * std::clamp(ratio, 0.0f, 1.0f), max.y - 3 * u};
  if (fillMax.x > fillMin.x) {
    drawList->AddRectFilled(fillMin, fillMax, IM_COL32(2, 125, 198, a(245)));
    drawList->AddRectFilled(fillMin, {fillMax.x, fillMin.y + 1.5f * u}, IM_COL32(20, 150, 225, a(200)));
    drawList->AddRectFilled({fillMin.x, fillMax.y - 1.5f * u}, fillMax, IM_COL32(10, 140, 215, a(255)));
  }
}

static void DrawLoadingArc(ImVec2 centre, float size, float alpha) {
  // The game's loading ring, spinning.
  if (!g_loadingArc) return;
  float rotation = float(-2 * M_PI * std::fmod(ImGui::GetTime() - g_installerStartTime, 1.0));
  float c = cosf(rotation), s = sinf(rotation), h = size / 2;
  auto corner = [&](float x, float y) { return ImVec2(centre.x + x * c - y * s, centre.y + x * s + y * c); };
  ImGui::GetBackgroundDrawList()->AddImageQuad(TexRef(g_loadingArc), corner(-h, -h), corner(h, -h),
                                               corner(h, h), corner(-h, h), {0, 0}, {1, 0}, {1, 1},
                                               {0, 1}, IM_COL32(120, 210, 255, int(220 * alpha)));
}

// The page's window, like the game's menus (rows) and message windows
// (text, a line, then the choices). Returns the description of the row under
// the cursor.
static const std::string* DrawWindow() {
  float alpha = UiAlpha();
  auto drawList = ImGui::GetBackgroundDrawList();
  std::vector<Row> rows = PageRows();
  std::string text = WindowText();

  float fontSize = Scale(BODY_FONT_SIZE);
  float lineMargin = LineMargin();
  float textWidth = Scale(WINDOW_X1 - WINDOW_X0 - TEXT_MARGIN_X * 2);

  // Height of each part, in layout units.
  float extraHeight = 0.0f;
  if (g_currentPage == WizardPage::SelectGame) extraHeight = LINE_HEIGHT + 8.0f;
  if (g_currentPage == WizardPage::Installing) extraHeight = 44.0f;
  if (g_currentPage == WizardPage::InstallSucceeded) extraHeight = 128.0f;
  float rowsHeight = rows.size() * ROW_HEIGHT;
  float textHeight = text.empty() ? 0.0f
                                  : MeasureCentredParagraph(g_bodyFont, fontSize, textWidth, lineMargin,
                                                            text.c_str()).y / g_aspectRatioScale;
  constexpr float contentTop = WINDOW_Y0 + 84.0f;
  float bottomPadding = 21.0f;
  float separatorHeight = (!text.empty() || extraHeight > 0) && !rows.empty() ? 14.0f + 3.0f + 17.0f : 0.0f;
  float textPadding = text.empty() ? 0.0f : 6.0f;
  float maxTextHeight = WINDOW_MAX_Y1 - bottomPadding - rowsHeight - separatorHeight - extraHeight -
                        contentTop - textPadding;
  float visibleTextHeight = std::min(textHeight, maxTextHeight);
  float windowY1 = contentTop + textPadding + visibleTextHeight + extraHeight + separatorHeight +
                   rowsHeight + bottomPadding;
  if (rows.empty()) windowY1 += 8.0f;

  ImVec2 bodyMin = LayoutPos(WINDOW_X0, WINDOW_Y0);
  ImVec2 bodyMax = LayoutPos(WINDOW_X1, windowY1);
  DrawStfPanel(bodyMin, bodyMax, alpha);
  DrawStfTab(LayoutPos(WINDOW_X0 - 9.0f, WINDOW_Y0 - 9.0f), alpha);

  // Title.
  auto& title = Localise(g_currentPage == WizardPage::Installing ? "Installer_Header_Installing"
                                                                 : "Installer_Header_Installer");
  InstallerFont* titleFont = HasWideCharacters(title.c_str()) ? g_bodyFont : g_titleFont;
  float titleSize = Scale(TITLE_FONT_SIZE);
  ImVec2 titleSizePx = titleFont->CalcTextSizeA(titleSize, FLT_MAX, 0, title.c_str());
  ImVec2 titlePos = {(bodyMin.x + bodyMax.x - titleSizePx.x) / 2, LayoutPos(0, WINDOW_Y0 + 34.0f).y - titleSizePx.y / 2};
  DrawTextBasic(titleFont, titleSize, {titlePos.x + Scale(1), titlePos.y + Scale(1.5f)},
                IM_COL32(0, 0, 0, int(90 * alpha)), title.c_str());
  DrawTextBasic(titleFont, titleSize, titlePos, IM_COL32(255, 255, 255, int(255 * alpha)), title.c_str());
  if (g_currentPage == WizardPage::Installing) {
    DrawLoadingArc(LayoutPos(WINDOW_X1 - 44.0f, WINDOW_Y0 + 34.0f), Scale(40.0f), alpha);
  }
  DrawStfRule(bodyMin.x, bodyMax.x, LayoutPos(0, WINDOW_Y0 + 66.0f).y, alpha);

  // Text: scrolls by itself when it doesn't fit (waiting at the top and the
  // bottom).
  float y = contentTop + textPadding;
  if (!text.empty()) {
    static std::string scrollText;
    static double scrollStart = 0.0;
    if (scrollText != text) {
      scrollText = text;
      scrollStart = ImGui::GetTime();
    }
    float overflow = Scale(textHeight - visibleTextHeight);
    float scrollY = 0.0f;
    ImVec2 clipMin = {bodyMin.x, LayoutPos(0, y).y - Scale(4)};
    ImVec2 clipMax = {bodyMax.x, LayoutPos(0, y + visibleTextHeight).y + Scale(2)};
    if (overflow > 0.0f) {
      constexpr double hold = 2.5;
      double speed = Scale(30.0f);
      double travel = overflow / speed;
      double t = fmod(ImGui::GetTime() - scrollStart, hold + travel + hold);
      scrollY = t < hold ? 0.0f : t < hold + travel ? float((t - hold) * speed) : overflow;
      float fade = Scale(24.0f);
      SetVerticalMarqueeFade(clipMin, clipMax, scrollY > 0.0f ? fade : 0.001f,
                             scrollY < overflow ? fade : 0.001f);
    }
    drawList->PushClipRect(clipMin, clipMax, true);
    ImVec2 pos = LayoutPos(WINDOW_X0 + TEXT_MARGIN_X, y);
    bool japanese = Config::Language == ELanguage::Japanese;
    if (japanese) pos.y += fontSize * ANNOTATION_FONT_SIZE_MODIFIER * 0.8f;
    DrawRubyAnnotatedText(
        g_bodyFont, fontSize, textWidth, {pos.x, pos.y - scrollY}, lineMargin, text.c_str(),
        [=](const char* str, ImVec2 p) {
          DrawTextBasic(g_bodyFont, fontSize, p, IM_COL32(255, 255, 255, int(255 * alpha)), str);
        },
        [=](const char* str, float size, ImVec2 p) {
          DrawTextBasic(g_bodyFont, size, p, IM_COL32(255, 255, 255, int(255 * alpha)), str);
        },
        false, japanese);
    drawList->PopClipRect();
    if (overflow > 0.0f) ResetMarqueeFade();
    y += visibleTextHeight;
  }

  // Page extras.
  if (g_currentPage == WizardPage::SelectGame) {
    // The game's source, with a light that comes on once it's added.
    y += 8.0f;
    auto& label = Localise("Installer_Step_Game");
    ImVec2 lightPos = LayoutPos(WINDOW_X0 + TEXT_MARGIN_X, y + (LINE_HEIGHT - 14.0f) / 2);
    DrawToggleLight(lightPos, !g_gameSourcePath.empty(), alpha);
    DrawTextBasic(g_bodyFont, fontSize, LayoutPos(WINDOW_X0 + TEXT_MARGIN_X + 26.0f, y + (LINE_HEIGHT - BODY_FONT_SIZE) / 2),
                  IM_COL32(255, 255, 255, int(255 * alpha)), label.c_str());
    if (!g_gameSourcePath.empty()) {
      std::u8string name = g_gameSourcePath.filename().u8string();
      std::string file = Truncate(std::string(name.begin(), name.end()), 28, true, true);
      auto size = g_bodyFont->CalcTextSizeA(fontSize, FLT_MAX, 0, file.c_str());
      ImVec2 pos = LayoutPos(WINDOW_X1 - TEXT_MARGIN_X, y + (LINE_HEIGHT - BODY_FONT_SIZE) / 2);
      DrawTextBasic(g_bodyFont, fontSize, {pos.x - size.x, pos.y}, IM_COL32(150, 215, 255, int(255 * alpha)), file.c_str());
    }
    y += LINE_HEIGHT;
  } else if (g_currentPage == WizardPage::Installing) {
    y += 12.0f;
    DrawProgressBar(LayoutPos(WINDOW_X0 + TEXT_MARGIN_X, y), LayoutPos(WINDOW_X1 - TEXT_MARGIN_X, y + 24.0f),
                    g_installerProgressRatioCurrent, alpha);
    y += 32.0f;
  } else if (g_currentPage == WizardPage::InstallSucceeded) {
    // The project's logo (UnleashedRecomp: the hedge-dev logo and name) and
    // the credits going by.
    y += 6.0f;
    if (g_projectLogo) {
      float logoH = 80.0f, logoW = logoH * g_projectLogo->width / g_projectLogo->height;
      float centreX = (WINDOW_X0 + WINDOW_X1) / 2;
      drawList->AddImage(TexRef(g_projectLogo), LayoutPos(centreX - logoW / 2, y),
                         LayoutPos(centreX + logoW / 2, y + logoH), {0, 0}, {1, 1},
                         IM_COL32(255, 255, 255, int(255 * alpha)));
    }
    y += 86.0f;
    ImVec2 marqueeMin = LayoutPos(WINDOW_X0 + 12.0f, y);
    ImVec2 marqueeMax = LayoutPos(WINDOW_X1 - 12.0f, y + LINE_HEIGHT);
    SetHorizontalMarqueeFade(marqueeMin, marqueeMax, Scale(32));
    DrawTextWithMarquee(g_bodyFont, fontSize, {marqueeMax.x, marqueeMin.y + Scale(LINE_HEIGHT - BODY_FONT_SIZE) / 2},
                        marqueeMin, marqueeMax, IM_COL32(255, 255, 255, int(255 * alpha)),
                        g_creditsStr.c_str(), g_installerEndTime, 0.9, Scale(200));
    ResetMarqueeFade();
    y += LINE_HEIGHT;
  }

  // The choices.
  if (separatorHeight > 0.0f) {
    y += 14.0f;
    DrawStfRule(bodyMin.x, bodyMax.x, LayoutPos(0, y).y, alpha);
    y += 3.0f + 17.0f;
  } else if (!rows.empty() && text.empty()) {
    y += 1.0f;
  }

  bool interactive = g_currentMessagePrompt.empty() && !g_currentPickerVisible && alpha >= 1.0f &&
                     !g_isDisappearing;
  static WizardPage cursorPage = WizardPage(-1);
  if (cursorPage != g_currentPage) {
    cursorPage = g_currentPage;
    g_currentCursorIndex = -1;
  }

  const std::string* description = nullptr;
  const Row* pressedRow = nullptr;
  int cursorIndex = 0;
  bool firstEnabled = true;
  for (size_t i = 0; i < rows.size(); ++i) {
    const Row& row = rows[i];
    ImVec2 min = {bodyMin.x + Scale(1), LayoutPos(0, y + i * ROW_HEIGHT).y};
    ImVec2 max = {bodyMax.x - Scale(1), LayoutPos(0, y + (i + 1) * ROW_HEIGHT).y};
    if (i % 2 == 0) {
      drawList->AddRectFilled(min, max, IM_COL32(0, 51, 125, int(64 * alpha)));
    }
    bool selected = false;
    if (row.enabled && interactive) {
      bool pressed = false;
      bool makeDefault = firstEnabled;
      firstEnabled = false;
      if (makeDefault && g_currentCursorIndex < 0) {
        // The game's menus always show a selection.
        g_currentCursorIndex = cursorIndex;
      }
      selected = PushCursorRect(min, max, pressed, makeDefault);
      ++cursorIndex;
      if (pressed) pressedRow = &row;
    }
    if (selected) {
      DrawSelectionContainer(min, max, alpha);
      description = row.description;
    }
    auto size = g_bodyFont->CalcTextSizeA(fontSize, FLT_MAX, 0, row.label.c_str());
    ImU32 colour = row.enabled ? IM_COL32(255, 255, 255, int(255 * alpha)) : IM_COL32(120, 130, 150, int(255 * alpha));
    DrawTextBasic(g_bodyFont, fontSize, {(min.x + max.x - size.x) / 2, (min.y + max.y - size.y) / 2},
                  colour, row.label.c_str());
  }

  if (!description) {
    if (g_currentPage == WizardPage::Installing) {
      description = &Localise("Installer_Desc_Installing");
    } else if (g_currentPage == WizardPage::SelectLanguage) {
      description = &Localise("Installer_Page_SelectLanguage");
    } else if (!rows.empty()) {
      description = rows.back().description;
    }
  }

  if (pressedRow) {
    DoRowAction(*pressedRow);
  }
  return description;
}

static void DrawDescriptionPanel(const std::string* description) {
  // The panel along the bottom of the game's menus that describes the
  // selected choice.
  float alpha = UiAlpha();
  auto drawList = ImGui::GetBackgroundDrawList();
  ImVec2 min = LayoutPos(DESCRIPTION_X0, DESCRIPTION_Y0);
  ImVec2 max = LayoutPos(DESCRIPTION_X1, DESCRIPTION_Y1);
  DrawStfPanel(min, max, alpha);
  if (!description || description->empty()) return;

  float fontSize = Scale(BODY_FONT_SIZE);
  float lineMargin = LineMargin();
  ImVec2 textMin = LayoutPos(DESCRIPTION_X0 + 55.0f, DESCRIPTION_Y0 + 21.0f);
  ImVec2 clipMax = LayoutPos(DESCRIPTION_X1 - 30.0f, DESCRIPTION_Y1 - 12.0f);
  float width = clipMax.x - textMin.x;
  bool japanese = Config::Language == ELanguage::Japanese;
  float height = MeasureCentredParagraph(g_bodyFont, fontSize, width, lineMargin, description->c_str()).y;

  static const std::string* scrollDescription = nullptr;
  static double scrollStart = 0.0;
  if (scrollDescription != description) {
    scrollDescription = description;
    scrollStart = ImGui::GetTime();
  }
  ImVec2 clipMin = {textMin.x - Scale(8), textMin.y - Scale(12)};
  float overflow = height - (clipMax.y - textMin.y);
  float scrollY = 0.0f;
  if (overflow > 0.0f) {
    constexpr double hold = 2.5;
    double speed = Scale(30.0f);
    double travel = overflow / speed;
    double t = fmod(ImGui::GetTime() - scrollStart, hold + travel + hold);
    scrollY = t < hold ? 0.0f : t < hold + travel ? float((t - hold) * speed) : overflow;
    float fade = Scale(18.0f);
    SetVerticalMarqueeFade(clipMin, clipMax, scrollY > 0.0f ? fade : 0.001f, scrollY < overflow ? fade : 0.001f);
  }
  drawList->PushClipRect(clipMin, clipMax, true);
  ImVec2 pos = {textMin.x, textMin.y - scrollY};
  if (japanese) pos.y += fontSize * ANNOTATION_FONT_SIZE_MODIFIER * 0.5f;
  DrawRubyAnnotatedText(
      g_bodyFont, fontSize, width, pos, lineMargin, description->c_str(),
      [=](const char* str, ImVec2 p) {
        DrawTextBasic(g_bodyFont, fontSize, p, IM_COL32(255, 255, 255, int(255 * alpha)), str);
      },
      [=](const char* str, float size, ImVec2 p) {
        DrawTextBasic(g_bodyFont, size, p, IM_COL32(255, 255, 255, int(255 * alpha)), str);
      },
      false, japanese);
  drawList->PopClipRect();
  if (overflow > 0.0f) ResetMarqueeFade();
}

static void OpenButtonGuide() {
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

  if (UiAlpha() < 1.0f || !g_currentMessagePrompt.empty()) {
    if (g_currentMessagePrompt.empty()) ButtonGuide::Close();
    return;
  }
  if (g_currentPage == WizardPage::InstallSucceeded) {
    ButtonGuide::Open(Button("Common_Select", FLT_MAX, selectIcon));
  } else if (g_currentPage == WizardPage::Installing) {
    ButtonGuide::Open(Button("Common_Cancel", FLT_MAX, backIcon));
  } else {
    const char* backKey = "Common_Back";
    if ((g_currentPage == g_firstPage) || (g_currentPage == WizardPage::InstallFailed)) {
      backKey = "Common_Quit";
    }
    std::array<Button, 2> buttons = {Button("Common_Select", FLT_MAX, selectIcon),
                                     Button(backKey, FLT_MAX, backIcon)};
    ButtonGuide::Open(buttons);
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
    // With the game added, go on to Next (after Add Files and Add Folder).
    if (!g_gameSourcePath.empty()) g_currentCursorIndex = 2;
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
  g_loadingArc = InstallerAssets::Texture("loading_arc");
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
  UpdateInstallingProgress();
  ResetCursorRects();
  DrawBackground();
  DrawCard();
  DrawBottomBand();
  // Like the game, a message window takes the place of the page's window.
  DrawDescriptionPanel(MessageWindow::s_isVisible ? nullptr : DrawWindow());
  OpenButtonGuide();
  CheckCancelAction();
  DrawMessagePrompt();
  PickerDrawForeground();
  PickerCheckTutorial();
  PickerCheckResults();
  MessageWindow::Draw();
  ButtonGuide::Draw();

  if (g_isDisappearing) {
    double disappearDuration = DISAPPEAR_DURATION;
    if (g_isQuitting) {
      // Add some extra waiting time when quitting the application altogether.
      disappearDuration += QUITTING_EXTRA_DURATION;
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
