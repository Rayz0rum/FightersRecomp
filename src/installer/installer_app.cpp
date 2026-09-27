// Shows the installer (installer_wizard) before the game starts when its data
// isn't installed yet, then continues starting the game with it.

#include "generated/default/stf_xbla_init.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>

#include "../stf_xbla_app.h"
#include "installer.h"
#include "installer_common.h"
#include "installer_wizard.h"

namespace {

// Default controls for a new install (the keyboard and mouse as a gamepad).
constexpr const char* kDefaultConfig =
    "# Default controls for a new install.\n"
    "# Let the keyboard and mouse work as a gamepad.\n"
    "mnk_mode = true\n"
    "mnk_mouse = false\n"
    "input_backend = \"sdl\"\n"
    "\n"
    "keybind_lstick_up = \"W\"\n"
    "keybind_lstick_down = \"S\"\n"
    "keybind_lstick_left = \"A\"\n"
    "keybind_lstick_right = \"D\"\n"
    "keybind_a = \"Space\"\n"
    "keybind_b = \"C\"\n"
    "keybind_x = \"Z\"\n"
    "keybind_y = \"X\"\n"
    "keybind_start = \"Return\"\n"
    "keybind_back = \"Backspace\"\n"
    "keybind_left_shoulder = \"Q\"\n"
    "keybind_right_shoulder = \"E\"\n"
    "keybind_left_trigger = \"Shift+Shift\"\n"
    "keybind_right_trigger = \"R\"\n";

// Draws the wizard every frame until it's done.
class InstallerDialog final : public rex::ui::ImGuiDialog {
 public:
  InstallerDialog(rex::ui::ImGuiDrawer* drawer, std::function<void(bool quit)> done)
      : ImGuiDialog(drawer), done_(std::move(done)) {}

 protected:
  void OnDraw(ImGuiIO&) override {
    InstallerWizard::Draw();
    if (!InstallerWizard::s_isVisible && !closing_) {
      closing_ = true;
      Close();
    }
  }

  void OnClose() override {
    bool quit = InstallerWizard::IsQuitting();
    InstallerWizard::Shutdown();
    done_(quit);
  }

 private:
  std::function<void(bool)> done_;
  bool closing_ = false;
};

void WriteNewInstallConfig(const std::filesystem::path& config_path) {
  std::error_code error;
  bool exists = std::filesystem::exists(config_path, error);
  if (!exists) {
    std::ofstream(config_path) << kDefaultConfig;
  }
  // The game has English and Japanese text - Japanese if chosen.
  if (Config::Language == ELanguage::Japanese) {
    std::ofstream(config_path, std::ios::app) << "\nuser_language = 2\n";
  }
  rex::cvar::LoadConfig(config_path);
}

}  // namespace

std::optional<rex::PathConfig> StfXblaApp::OnFinalizePaths(
    const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) {
  if (Installer::checkInstall(defaults.game_data_root)) {
    return defaults;
  }
  rex::ui::ImmediateDrawer* drawer = immediate_drawer();
  rex::ui::ImGuiDrawer* imgui = imgui_drawer();
  std::filesystem::path install_path = defaults.game_data_root;
  if (!drawer || !imgui || !InstallerWizard::Init(*drawer, install_path)) {
    // Starting reports the missing game data.
    return defaults;
  }
  REXLOG_INFO("Game data not found - showing the installer");
  new InstallerDialog(imgui, [this, defaults, install_path, resume](bool quit) {
    if (quit) {
      app_context().CallInUIThreadDeferred([this] { app_context().QuitFromUIThread(); });
      return;
    }
    WriteNewInstallConfig(defaults.config_path);
    rex::PathConfig paths = defaults;
    paths.game_data_root = install_path;
    // Start the game after this frame is drawn.
    app_context().CallInUIThreadDeferred([resume, paths] { resume(paths); });
  });
  return std::nullopt;
}

bool StfXblaApp::OnWindowCloseRequested() {
  return InstallerWizard::OnCloseRequested();
}
