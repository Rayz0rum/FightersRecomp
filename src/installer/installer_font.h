// Text drawing with the game's own fonts, as signed distance fields (see
// tools/installer_assets), for the installer's ImGui drawing. Mirrors the
// subset of ImFont that UnleashedRecomp's UI code uses.
#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <imgui.h>

#include <rex/ui/immediate_drawer.h>

#include "installer_pack.h"

class InstallerFont {
 public:
  // Creates the font from a pack entry (header, glyphs, SDF atlas).
  static std::unique_ptr<InstallerFont> Create(rex::ui::ImmediateDrawer& drawer,
                                               const uint8_t* data, uint32_t atlas_width,
                                               uint32_t atlas_height, float visual_scale);

  // The size glyph metrics are defined at (a line's height).
  float FontSize = 1.0f;

  // Advance of a character at FontSize.
  float GetCharAdvance(uint32_t c) const;

  ImVec2 CalcTextSizeA(float size, float max_width, float wrap_width, const char* text,
                       const char* text_end = nullptr) const;

  void AddText(ImDrawList* draw_list, float size, const ImVec2& pos, ImU32 color,
               const char* text, const char* text_end = nullptr) const;

 private:
  struct Glyph {
    float u0, v0, u1, v1;
    float offset_x, offset_y, w, h;
    float advance;
  };
  const Glyph* FindGlyph(uint32_t c) const;

  std::unique_ptr<rex::ui::ImmediateTexture> texture_;
  std::unordered_map<uint32_t, Glyph> glyphs_;
  float fallback_advance_ = 0.0f;
  // Rendered size relative to the requested one (the game's glyph cells have
  // more padding than regular fonts).
  float visual_scale_ = 1.0f;
};
