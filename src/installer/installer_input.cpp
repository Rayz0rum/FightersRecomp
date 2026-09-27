#include "installer_input.h"

#include <cmath>

#include <SDL3/SDL.h>

#include "installer_common.h"

namespace {

bool g_initialized = false;
SDL_Gamepad* g_gamepad = nullptr;
bool g_buttons[6] = {};
float g_axes[2] = {};

void OpenGamepad() {
  if (g_gamepad && SDL_GamepadConnected(g_gamepad)) {
    return;
  }
  if (g_gamepad) {
    SDL_CloseGamepad(g_gamepad);
    g_gamepad = nullptr;
  }
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  if (ids) {
    if (count > 0) {
      g_gamepad = SDL_OpenGamepad(ids[0]);
    }
    SDL_free(ids);
  }
}

bool IsPlayStation(SDL_Gamepad* gamepad) {
  switch (SDL_GetGamepadType(gamepad)) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
      return true;
    default:
      return false;
  }
}

}  // namespace

namespace InstallerInput {

void Init() {
  if (!g_initialized) {
    g_initialized = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
  }
}

void Shutdown() {
  if (g_gamepad) {
    SDL_CloseGamepad(g_gamepad);
    g_gamepad = nullptr;
  }
  if (g_initialized) {
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    g_initialized = false;
  }
}

std::vector<InstallerInputEvent> Poll() {
  using Event = InstallerInputEvent;
  std::vector<Event> events;
  ImGuiIO& io = ImGui::GetIO();

  // Keyboard.
  const std::pair<ImGuiKey, Event::Key> keys[] = {
      {ImGuiKey_LeftArrow, Event::KeyLeft},  {ImGuiKey_RightArrow, Event::KeyRight},
      {ImGuiKey_UpArrow, Event::KeyUp},      {ImGuiKey_DownArrow, Event::KeyDown},
      {ImGuiKey_Enter, Event::KeyReturn},    {ImGuiKey_KeypadEnter, Event::KeyReturn},
      {ImGuiKey_Escape, Event::KeyEscape},
  };
  for (auto [imgui_key, key] : keys) {
    if (ImGui::IsKeyPressed(imgui_key, true)) {
      events.push_back({Event::Type::KeyDown, key});
      hid::g_inputDevice = hid::EInputDevice::Keyboard;
    }
  }

  // Mouse.
  if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
    if (io.MousePos.x > -FLT_MAX) {
      events.push_back({Event::Type::MouseMotion});
      hid::g_inputDevice = hid::EInputDevice::Mouse;
    }
  }
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    events.push_back({Event::Type::MouseButtonDown});
    hid::g_inputDevice = hid::EInputDevice::Mouse;
  }

  // Controller.
  if (g_initialized) {
    SDL_UpdateGamepads();
    OpenGamepad();
    if (g_gamepad) {
      const SDL_GamepadButton buttons[6] = {
          SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_UP,
          SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_SOUTH,      SDL_GAMEPAD_BUTTON_EAST,
      };
      bool used = false;
      for (int i = 0; i < 6; ++i) {
        bool down = SDL_GetGamepadButton(g_gamepad, buttons[i]);
        if (down && !g_buttons[i]) {
          events.push_back({Event::Type::ControllerButton, i});
          used = true;
        }
        g_buttons[i] = down;
      }
      const SDL_GamepadAxis axes[2] = {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY};
      for (int i = 0; i < 2; ++i) {
        float value = SDL_GetGamepadAxis(g_gamepad, axes[i]) / 32767.0f;
        if (std::abs(value - g_axes[i]) > 0.01f) {
          events.push_back({Event::Type::ControllerAxis, i, value});
          if (std::abs(value) > 0.5f) used = true;
        }
        g_axes[i] = value;
      }
      if (used) {
        hid::g_inputDevice =
            IsPlayStation(g_gamepad) ? hid::EInputDevice::PlayStation : hid::EInputDevice::Xbox;
      }
    }
  }
  return events;
}

}  // namespace InstallerInput
