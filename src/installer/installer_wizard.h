// The installer, ported from UnleashedRecomp's ui/installer_wizard (hedge-dev,
// GPL-3.0) and themed after Sonic the Fighters.
#pragma once

#include <filesystem>

#include <rex/ui/immediate_drawer.h>

struct InstallerWizard {
  static inline bool s_isVisible = false;

  // Loads the assets and starts the wizard; false if it can't be shown.
  static bool Init(rex::ui::ImmediateDrawer& drawer, std::filesystem::path installPath);
  // Draws a frame (and handles its input) while s_isVisible.
  static void Draw();
  // After the wizard is no longer visible: whether it was closed to quit
  // rather than after installing.
  static bool IsQuitting();
  // The window's close button: during the installation, asks whether to cancel
  // it instead (returns false to keep the window).
  static bool OnCloseRequested();
  static void Shutdown();
};
