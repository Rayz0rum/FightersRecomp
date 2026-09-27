/**
 * @file        rex/ui/imgui_effects.h
 * @brief       Shader effects for ImGui draw lists, applied by ImGuiDrawer
 *
 * Draw list callbacks with these codes set ImmediateEffect state for the
 * following draws of the list: gradients, procedural patterns, text outlines
 * and bevels, transforms, marquee fades and additive blending. The design
 * (and the callback codes) follow UnleashedRecomp's ImGui callbacks
 * (hedge-dev, GPL-3.0).
 */

#pragma once

#include <cstdint>

#include <imgui.h>

namespace rex::ui {

enum class ImGuiEffectCallback : intptr_t {
  kSetGradient = -1,
  kSetShaderModifier = -2,
  kSetOrigin = -3,
  kSetScale = -4,
  kSetMarqueeFade = -5,
  kSetOutline = -6,
  kSetProceduralOrigin = -7,
  // -8 is ImDrawCallback_ResetRenderState (resets all the effects).
  kSetAdditive = -9,
};

union ImGuiEffectCallbackData {
  struct {
    float bounds_min[2];
    float bounds_max[2];
    uint32_t gradient_top_left;
    uint32_t gradient_top_right;
    uint32_t gradient_bottom_right;
    uint32_t gradient_bottom_left;
  } set_gradient;
  struct {
    uint32_t shader_modifier;
  } set_shader_modifier;
  struct {
    float origin[2];
  } set_origin;
  struct {
    float scale[2];
  } set_scale;
  struct {
    float bounds_min[2];
    float bounds_max[2];
  } set_marquee_fade;
  struct {
    float outline;
  } set_outline;
  struct {
    float procedural_origin[2];
  } set_procedural_origin;
  struct {
    bool enabled;
  } set_additive;
};

// Adds an effect callback to the draw list (the data is copied).
inline void AddImGuiEffectCallback(ImDrawList* draw_list, ImGuiEffectCallback callback,
                                   const ImGuiEffectCallbackData& data) {
  draw_list->AddCallback(reinterpret_cast<ImDrawCallback>(static_cast<intptr_t>(callback)),
                         const_cast<ImGuiEffectCallbackData*>(&data), sizeof(data));
}

}  // namespace rex::ui
