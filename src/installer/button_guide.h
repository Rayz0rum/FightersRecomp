// Button prompts at the bottom of the screen, ported from UnleashedRecomp's
// ui/button_guide (hedge-dev, GPL-3.0). Controller prompts use Sonic the
// Fighters' own button glyphs (Xbox or PlayStation), keyboard and mouse ones
// UnleashedRecomp's icons.
#pragma once

#include <cfloat>
#include <span>
#include <string>

enum class EButtonIcon {
  // Controller
  A,
  B,
  X,
  Y,
  LB,
  RB,
  LT,
  RT,
  Start,
  Back,

  // Keyboard + Mouse
  LMB,
  Enter,
  Escape
};

enum class EButtonAlignment { Left, Right };

class Button {
 public:
  std::string Name{};
  float MaxWidth{FLT_MAX};
  EButtonIcon Icon{};
  EButtonAlignment Alignment{EButtonAlignment::Right};
  bool* Visibility{nullptr};

  Button(std::string name, float maxWidth, EButtonIcon icon, EButtonAlignment alignment,
         bool* visibility = nullptr)
      : Name(name), MaxWidth(maxWidth), Icon(icon), Alignment(alignment), Visibility(visibility) {}

  Button(std::string name, float maxWidth, EButtonIcon icon)
      : Name(name), MaxWidth(maxWidth), Icon(icon) {}
};

class ButtonGuide {
 public:
  static inline bool s_isVisible = false;

  static void Draw();
  static void Open(Button button);
  static void Open(const std::span<Button> buttons);
  static void SetSideMargins(float width);
  static void Close();
};
