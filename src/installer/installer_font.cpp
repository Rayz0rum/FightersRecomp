#include "installer_font.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

// Decodes one UTF-8 character, returning its length.
int DecodeUtf8(const char* s, const char* end, uint32_t& c) {
  auto b = uint8_t(s[0]);
  int n = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
  if (s + n > end) {
    c = 0xFFFD;
    return 1;
  }
  c = n == 1 ? b : n == 2 ? b & 0x1F : n == 3 ? b & 0x0F : b & 0x07;
  for (int i = 1; i < n; ++i) {
    c = (c << 6) | (uint8_t(s[i]) & 0x3F);
  }
  return n;
}

}  // namespace

std::unique_ptr<InstallerFont> InstallerFont::Create(rex::ui::ImmediateDrawer& drawer,
                                                     const uint8_t* data, uint32_t atlas_width,
                                                     uint32_t atlas_height, float visual_scale) {
  auto font = std::unique_ptr<InstallerFont>(new InstallerFont());
  InstallerFontHeader header;
  std::memcpy(&header, data, sizeof(header));
  font->FontSize = header.line_height;
  font->visual_scale_ = visual_scale;
  const auto* glyphs = reinterpret_cast<const InstallerFontGlyph*>(data + sizeof(header));
  for (uint32_t i = 0; i < header.glyph_count; ++i) {
    const InstallerFontGlyph& g = glyphs[i];
    Glyph glyph;
    glyph.u0 = float(g.x) / atlas_width;
    glyph.v0 = float(g.y) / atlas_height;
    glyph.u1 = float(g.x + g.w) / atlas_width;
    glyph.v1 = float(g.y + g.h) / atlas_height;
    glyph.offset_x = g.offset_x;
    glyph.offset_y = g.offset_y;
    glyph.w = g.w;
    glyph.h = g.h;
    glyph.advance = g.advance;
    font->glyphs_[g.code] = glyph;
  }
  if (const Glyph* space = font->FindGlyph(' ')) {
    font->fallback_advance_ = space->advance;
  }
  // The single channel field, replicated to R8G8B8A8.
  const uint8_t* sdf = data + sizeof(header) + header.glyph_count * sizeof(InstallerFontGlyph);
  std::vector<uint8_t> rgba(size_t(atlas_width) * atlas_height * 4);
  for (size_t i = 0; i < size_t(atlas_width) * atlas_height; ++i) {
    rgba[i * 4 + 0] = sdf[i];
    rgba[i * 4 + 1] = sdf[i];
    rgba[i * 4 + 2] = sdf[i];
    rgba[i * 4 + 3] = 255;
  }
  font->texture_ = drawer.CreateTexture(atlas_width, atlas_height,
                                        rex::ui::ImmediateTextureFilter::kLinear, false,
                                        rgba.data());
  if (!font->texture_) {
    return nullptr;
  }
  font->texture_->flags |= rex::ui::ImmediateTexture::kFlagSignedDistanceField;
  return font;
}

const InstallerFont::Glyph* InstallerFont::FindGlyph(uint32_t c) const {
  auto it = glyphs_.find(c);
  if (it == glyphs_.end() && c == 0x00DF) {
    it = glyphs_.find('s');
  }
  return it == glyphs_.end() ? nullptr : &it->second;
}

float InstallerFont::GetCharAdvance(uint32_t c) const {
  const Glyph* glyph = FindGlyph(c);
  // The size here is the visual size.
  return (glyph ? glyph->advance : fallback_advance_) * visual_scale_;
}

ImVec2 InstallerFont::CalcTextSizeA(float size, float max_width, float wrap_width,
                                    const char* text, const char* text_end) const {
  (void)max_width;
  (void)wrap_width;
  if (!text_end) {
    text_end = text + std::strlen(text);
  }
  float scale = size / FontSize;
  float line_width = 0.0f, width = 0.0f;
  int lines = 1;
  for (const char* s = text; s < text_end;) {
    uint32_t c;
    s += DecodeUtf8(s, text_end, c);
    if (c == '\n') {
      width = std::max(width, line_width);
      line_width = 0.0f;
      ++lines;
      continue;
    }
    if (c == '\r') {
      continue;
    }
    line_width += GetCharAdvance(c) * scale;
  }
  width = std::max(width, line_width);
  return ImVec2(width, size * lines);
}

void InstallerFont::AddText(ImDrawList* draw_list, float size, const ImVec2& pos, ImU32 color,
                            const char* text, const char* text_end) const {
  if (!text_end) {
    text_end = text + std::strlen(text);
  }
  if ((color & IM_COL32_A_MASK) == 0) {
    return;
  }
  float scale = size / FontSize;
  // Glyph boxes are scaled around the line's vertical centre.
  float glyph_scale = scale * visual_scale_;
  float centre_offset = size * 0.5f * (1.0f - visual_scale_);
  draw_list->PushTexture(ImTextureRef(ImTextureID(reinterpret_cast<uintptr_t>(texture_.get()))));
  float x = pos.x, y = pos.y;
  for (const char* s = text; s < text_end;) {
    uint32_t c;
    s += DecodeUtf8(s, text_end, c);
    if (c == '\n') {
      x = pos.x;
      y += size;
      continue;
    }
    if (c == '\r') {
      continue;
    }
    const Glyph* glyph = FindGlyph(c);
    if (!glyph) {
      x += fallback_advance_ * glyph_scale;
      continue;
    }
    float x0 = x + glyph->offset_x * glyph_scale;
    float y0 = y + centre_offset + glyph->offset_y * glyph_scale;
    float x1 = x0 + glyph->w * glyph_scale;
    float y1 = y0 + glyph->h * glyph_scale;
    draw_list->PrimReserve(6, 4);
    draw_list->PrimRectUV(ImVec2(x0, y0), ImVec2(x1, y1), ImVec2(glyph->u0, glyph->v0),
                          ImVec2(glyph->u1, glyph->v1), color);
    x += glyph->advance * glyph_scale;
  }
  draw_list->PopTexture();
}
