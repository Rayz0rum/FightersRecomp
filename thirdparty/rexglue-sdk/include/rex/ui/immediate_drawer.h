#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <memory>

#include <rex/ui/presenter.h>

namespace rex {
namespace ui {

// Describes the filter applied when sampling textures.
enum class ImmediateTextureFilter {
  kNearest,
  kLinear,
};

// Simple texture compatible with the immediate renderer.
class ImmediateTexture {
 public:
  virtual ~ImmediateTexture() = default;

  // Texture width, in pixels.
  uint32_t width;
  // Texture height, in pixels.
  uint32_t height;

  enum : uint32_t {
    // Holds a signed distance field (in R, 0.5 at the edge) rather than
    // colors - drawn as text with ImmediateEffect.
    kFlagSignedDistanceField = 1u << 0,
  };
  uint32_t flags = 0;

 protected:
  ImmediateTexture(uint32_t width, uint32_t height) : width(width), height(height) {}
};

// Describes the primitive type used by a draw call.
enum class ImmediatePrimitiveType {
  kLines,
  kTriangles,
};

// Simple vertex used by the immediate mode drawer.
// To avoid translations, this matches both imgui and elemental-forms vertices:
//   ImDrawVert
//   el::graphics::Renderer::Vertex
struct ImmediateVertex {
  float x, y;
  float u, v;
  uint32_t color;
};

// All parameters required to draw an immediate-mode batch of vertices.
struct ImmediateDrawBatch {
  // Vertices to draw.
  const ImmediateVertex* vertices = nullptr;
  int vertex_count = 0;

  // Optional index buffer indices.
  const uint16_t* indices = nullptr;
  int index_count = 0;
};

// Shader effects of a draw (UnleashedRecomp-style ImGui effects: gradients,
// procedural patterns, text outlines and bevels, transforms). Must match the
// EffectConstants buffer of the effect shaders.
struct ImmediateEffect {
  enum : uint32_t {
    kModifierNone = 0,
    kModifierScanline = 1,
    kModifierCheckerboard = 2,
    kModifierScanlineButton = 3,
    kModifierTextSkew = 4,
    kModifierHorizontalMarqueeFade = 5,
    kModifierVerticalMarqueeFade = 6,
    kModifierGrayscale = 7,
    kModifierTitleBevel = 8,
    kModifierCategoryBevel = 9,
    kModifierRectangleBevel = 10,
    kModifierLowQualityText = 11,
  };
  enum : uint32_t {
    kTextureFlagSignedDistanceField = 1u << 0,
  };

  // Gradient (or marquee fade) bounds in the coordinate space - no gradient
  // if equal.
  float bounds_min[2] = {};
  float bounds_max[2] = {};
  // R8G8B8A8 (little-endian) gradient corner colors.
  uint32_t gradient_top_left = 0;
  uint32_t gradient_top_right = 0;
  uint32_t gradient_bottom_right = 0;
  uint32_t gradient_bottom_left = 0;
  uint32_t shader_modifier = kModifierNone;
  uint32_t texture_flags = 0;
  // Filled by the drawer.
  float display_size[2] = {};
  float inverse_display_size[2] = {};
  // Vertex transform: origin + (position - origin) * scale.
  float origin[2] = {};
  float scale[2] = {1.0f, 1.0f};
  // Origin of the procedural patterns.
  float procedural_origin[2] = {};
  // Signed distance field text outline, in the SDF's pixel range units.
  float outline = 0.0f;
  float padding0 = 0.0f;
  // Filled by the drawer.
  float pixel_to_coordinates[2] = {};

  // Whether drawing with these is the same as without effects.
  bool IsIdentity() const {
    return bounds_min[0] == bounds_max[0] && bounds_min[1] == bounds_max[1] &&
           shader_modifier == kModifierNone && texture_flags == 0 && scale[0] == 1.0f &&
           scale[1] == 1.0f && outline == 0.0f;
  }
};

struct ImmediateDraw {
  // Primitive type the vertices/indices represent.
  ImmediatePrimitiveType primitive_type = ImmediatePrimitiveType::kTriangles;
  // Total number of elements to draw.
  int count = 0;
  // Starting offset in the index buffer.
  int index_offset = 0;
  // Base vertex of elements, if using an index buffer.
  int base_vertex = 0;

  // Texture used when drawing, or nullptr if color only.
  ImmediateTexture* texture = nullptr;

  // True to enable scissoring using the region defined by scissor_rect.
  bool scissor = false;
  // Scissoring region in the coordinate space (if right < left or bottom < top,
  // not drawing).
  float scissor_left = 0.0f;
  float scissor_top = 0.0f;
  float scissor_right = 0.0f;
  float scissor_bottom = 0.0f;

  // Shader effects, or nullptr for plain drawing (not supported by every
  // implementation - drawn plainly then).
  const ImmediateEffect* effect = nullptr;
  // Additive rather than alpha blending (with effects).
  bool additive = false;
};

class ImmediateDrawer {
 public:
  ImmediateDrawer(const ImmediateDrawer& immediate_drawer) = delete;
  ImmediateDrawer& operator=(const ImmediateDrawer& immediate_drawer) = delete;

  virtual ~ImmediateDrawer() = default;

  void SetPresenter(Presenter* new_presenter);

  // Creates a new texture with the given attributes and R8G8B8A8 data.
  virtual std::unique_ptr<ImmediateTexture> CreateTexture(uint32_t width, uint32_t height,
                                                          ImmediateTextureFilter filter,
                                                          bool is_repeated,
                                                          const uint8_t* data) = 0;

  // Begins drawing in immediate mode using the given projection matrix. The
  // presenter that is currently attached to the immediate drawer, as the
  // implementation may hold presenter-specific information such as UI
  // submission indices. Pass 0 or a negative value as the coordinate space
  // width or height to use raw render target pixel coordinates (or this will
  // just be used as a safe fallback when with a non-zero-sized surface the
  // coordinate space size becomes zero somehow).
  virtual void Begin(UIDrawContext& ui_draw_context, float coordinate_space_width,
                     float coordinate_space_height);
  // Starts a draw batch.
  virtual void BeginDrawBatch(const ImmediateDrawBatch& batch) = 0;
  // Draws one set of a batch.
  virtual void Draw(const ImmediateDraw& draw) = 0;
  // Ends a draw batch.
  virtual void EndDrawBatch() = 0;
  // Ends drawing in immediate mode and flushes contents.
  virtual void End();

 protected:
  ImmediateDrawer() = default;

  Presenter* presenter() const { return presenter_; }
  virtual void OnLeavePresenter() {}
  virtual void OnEnterPresenter() {}

  // Available between Begin and End.
  UIDrawContext* ui_draw_context() const { return ui_draw_context_; }
  float coordinate_space_width() const { return coordinate_space_width_; }
  float coordinate_space_height() const { return coordinate_space_height_; }

  // Converts and clamps the scissor in the immediate draw to render target
  // coordinates. Returns whether the scissor contains any render target pixels
  // (but a valid scissor is written even if false is returned).
  bool ScissorToRenderTarget(const ImmediateDraw& immediate_draw, uint32_t& out_left,
                             uint32_t& out_top, uint32_t& out_width, uint32_t& out_height);

 private:
  Presenter* presenter_ = nullptr;

  UIDrawContext* ui_draw_context_ = nullptr;
  float coordinate_space_width_;
  float coordinate_space_height_;
};

}  // namespace ui
}  // namespace rex
