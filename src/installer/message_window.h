// Message boxes, ported from UnleashedRecomp's ui/message_window (hedge-dev,
// GPL-3.0).
#pragma once

#include <span>
#include <string>

#include "installer_input.h"

class MessageWindow {
 public:
  static inline bool s_isVisible = false;

  static void HandleInput(const InstallerInputEvent& event);
  static void Draw();
  static bool Open(std::string text, int* result, std::span<std::string> buttons = {},
                   int defaultButtonIndex = 0, int cancelButtonIndex = 1);
  static void Close();
};
