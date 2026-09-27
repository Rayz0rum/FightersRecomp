// Shared state of the installer UI: assets, localisation, sounds, input
// device and the 1280x720 layout scale, in the shape UnleashedRecomp's UI
// code expects (Localise, Config::Language, Game_PlaySound, hid, the aspect
// ratio globals).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <imgui.h>

#include <rex/ui/immediate_drawer.h>

class InstallerFont;

enum class ELanguage : uint32_t {
  English,
  Japanese,
  German,
  French,
  Spanish,
  Italian,
};

namespace Config {
extern ELanguage Language;
}

namespace hid {
enum class EInputDevice {
  Keyboard,
  Mouse,
  Xbox,
  PlayStation,
};
extern EInputDevice g_inputDevice;
bool IsInputDeviceController();
}  // namespace hid

// Layout of the 1280x720 design in the display (letterboxed or pillarboxed).
extern float g_aspectRatioOffsetX;
extern float g_aspectRatioOffsetY;
extern float g_aspectRatioScale;
void UpdateAspectRatio(const ImVec2& display_size);

const std::string& Localise(std::string_view key);
const std::string& Localise(std::string_view key, ELanguage language);
extern std::string g_localeMissing;

enum class InstallerSound {
  Cursor,
  Decide,
  Cancel,
  Ring,
};
void Game_PlaySound(InstallerSound sound);

using GuestTexture = rex::ui::ImmediateTexture;

inline ImTextureRef TexRef(const rex::ui::ImmediateTexture* texture) {
  return ImTextureRef(ImTextureID(reinterpret_cast<uintptr_t>(texture)));
}

// Installer assets, loaded from the embedded pack.
namespace InstallerAssets {
bool Load(rex::ui::ImmediateDrawer& drawer);
void Unload();

GuestTexture* Texture(std::string_view name);
InstallerFont* BodyFont();
InstallerFont* TitleFont();

// A controller button glyph of the game (installer_pack.h ids): texture and
// its UV rectangle and pixel size.
bool Button(uint32_t id, GuestTexture*& texture, ImVec2& uv_min, ImVec2& uv_max, ImVec2& size);

// Raw HCA data of a sound.
bool Sound(std::string_view name, const uint8_t*& data, size_t& size);
}  // namespace InstallerAssets
