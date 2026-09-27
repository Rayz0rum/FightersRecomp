// Input for the installer: keyboard and mouse from ImGui, controllers polled
// with SDL - as the events UnleashedRecomp's SDL listeners handled.
#pragma once

#include <vector>

#include <imgui.h>

struct InstallerInputEvent {
  enum class Type {
    KeyDown,           // key: one of the Key values
    ControllerButton,  // button: one of the Button values
    ControllerAxis,    // axis 0/1, value [-1, 1]
    MouseMotion,
    MouseButtonDown,
    Quit,
  };
  enum Key { KeyLeft, KeyRight, KeyUp, KeyDown, KeyReturn, KeyEscape };
  enum Button { DpadLeft, DpadRight, DpadUp, DpadDown, ButtonA, ButtonB };

  Type type;
  int code = 0;
  float value = 0.0f;
};

namespace InstallerInput {
void Init();
void Shutdown();
// This frame's events (and updates hid::g_inputDevice).
std::vector<InstallerInputEvent> Poll();
}  // namespace InstallerInput
