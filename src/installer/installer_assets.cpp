// Loads the installer's asset pack (built by tools/installer_assets from the
// game's files and embedded as the STF_INSTALLER_ASSETS resource).

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <compressapi.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <rex/logging.h>

#include "installer_common.h"
#include "installer_font.h"
#include "installer_pack.h"

namespace Config {
ELanguage Language = ELanguage::English;
}

namespace hid {
EInputDevice g_inputDevice = EInputDevice::Mouse;
bool IsInputDeviceController() {
  return g_inputDevice == EInputDevice::Xbox || g_inputDevice == EInputDevice::PlayStation;
}
}  // namespace hid

float g_aspectRatioOffsetX = 0.0f;
float g_aspectRatioOffsetY = 0.0f;
float g_aspectRatioScale = 1.0f;

void UpdateAspectRatio(const ImVec2& display_size) {
  float width = std::max(display_size.x, 1.0f), height = std::max(display_size.y, 1.0f);
  if (width * 9.0f >= height * 16.0f) {
    // Wider than 16:9 - pillarboxed.
    g_aspectRatioScale = height / 720.0f;
    g_aspectRatioOffsetX = (width - 1280.0f * g_aspectRatioScale) * 0.5f;
    g_aspectRatioOffsetY = 0.0f;
  } else {
    g_aspectRatioScale = width / 1280.0f;
    g_aspectRatioOffsetX = 0.0f;
    g_aspectRatioOffsetY = (height - 720.0f * g_aspectRatioScale) * 0.5f;
  }
}

std::string g_localeMissing = "???";

namespace {

struct Pack {
  std::vector<uint8_t> data;
  std::map<std::string, const InstallerPackEntry*, std::less<>> entries;
  std::map<std::string, std::unique_ptr<GuestTexture>, std::less<>> textures;
  std::unique_ptr<InstallerFont> body_font, title_font;
  std::unique_ptr<GuestTexture> buttons;
  std::map<uint32_t, InstallerSprite> button_sprites;
  // [key][language]
  std::map<std::string, std::map<std::string, std::string>, std::less<>> strings;
};
std::unique_ptr<Pack> g_pack;

std::string UnescapeLine(std::string_view value) {
  std::string out;
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '\\' && i + 1 < value.size()) {
      char next = value[i + 1];
      if (next == 'n') {
        out += '\n';
        ++i;
        continue;
      }
      if (next == '\\') {
        out += '\\';
        ++i;
        continue;
      }
    }
    out += value[i];
  }
  return out;
}

void ParseStrings(std::string_view text) {
  std::string key;
  size_t p = 0;
  while (p < text.size()) {
    size_t end = text.find('\n', p);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(p, end - p);
    p = end + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty() || line[0] == '#') continue;
    if (line[0] == '[' && line.back() == ']') {
      key = std::string(line.substr(1, line.size() - 2));
      continue;
    }
    size_t eq = line.find('=');
    if (eq == std::string_view::npos || key.empty()) continue;
    g_pack->strings[key][std::string(line.substr(0, eq))] = UnescapeLine(line.substr(eq + 1));
  }
}

const char* LanguageCode(ELanguage language) {
  switch (language) {
    case ELanguage::Japanese: return "ja";
    case ELanguage::German: return "de";
    case ELanguage::French: return "fr";
    case ELanguage::Spanish: return "es";
    case ELanguage::Italian: return "it";
    default: return "en";
  }
}

}  // namespace

const std::string& Localise(std::string_view key) {
  if (!g_pack) return g_localeMissing;
  auto it = g_pack->strings.find(key);
  if (it == g_pack->strings.end()) return g_localeMissing;
  auto lang = it->second.find(LanguageCode(Config::Language));
  if (lang == it->second.end()) lang = it->second.find("en");
  return lang == it->second.end() ? g_localeMissing : lang->second;
}

namespace InstallerAssets {

bool Load(rex::ui::ImmediateDrawer& drawer) {
  if (g_pack) return true;
  HMODULE module = GetModuleHandleW(nullptr);
  HRSRC resource = FindResourceW(module, L"STF_INSTALLER_ASSETS", MAKEINTRESOURCEW(10));
  HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
  const auto* blob = loaded ? static_cast<const uint8_t*>(LockResource(loaded)) : nullptr;
  DWORD blob_size = resource ? SizeofResource(module, resource) : 0;
  InstallerPackBlobHeader header;
  if (!blob || blob_size < sizeof(header)) {
    REXLOG_ERROR("Installer: the asset pack isn't embedded (build with the game files present)");
    return false;
  }
  std::memcpy(&header, blob, sizeof(header));
  if (std::memcmp(header.magic, "STFZ", 4) != 0 ||
      sizeof(header) + header.compressed_size > blob_size) {
    REXLOG_ERROR("Installer: invalid asset pack");
    return false;
  }
  auto pack = std::make_unique<Pack>();
  pack->data.resize(header.uncompressed_size);
  DECOMPRESSOR_HANDLE decompressor = nullptr;
  SIZE_T decompressed = 0;
  bool ok = CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &decompressor) &&
            Decompress(decompressor, blob + sizeof(header), header.compressed_size,
                       pack->data.data(), pack->data.size(), &decompressed) &&
            decompressed == header.uncompressed_size;
  if (decompressor) CloseDecompressor(decompressor);
  InstallerPackHeader pack_header;
  if (!ok || pack->data.size() < sizeof(pack_header)) {
    REXLOG_ERROR("Installer: can't decompress the asset pack");
    return false;
  }
  std::memcpy(&pack_header, pack->data.data(), sizeof(pack_header));
  if (std::memcmp(pack_header.magic, "STFI", 4) != 0 ||
      pack_header.version != kInstallerPackVersion) {
    REXLOG_ERROR("Installer: unsupported asset pack");
    return false;
  }
  const auto* entries =
      reinterpret_cast<const InstallerPackEntry*>(pack->data.data() + sizeof(pack_header));
  g_pack = std::move(pack);
  for (uint32_t i = 0; i < pack_header.entry_count; ++i) {
    const InstallerPackEntry& e = entries[i];
    const uint8_t* data = g_pack->data.data() + e.offset;
    g_pack->entries[e.name] = &e;
    switch (InstallerPackType(e.type)) {
      case InstallerPackType::kImage:
        g_pack->textures[e.name] =
            drawer.CreateTexture(e.width, e.height, rex::ui::ImmediateTextureFilter::kLinear,
                                 false, data);
        break;
      case InstallerPackType::kFont:
        if (std::string_view(e.name) == "font_body") {
          g_pack->body_font = InstallerFont::Create(drawer, data, e.width, e.height, 1.18f);
        } else if (std::string_view(e.name) == "font_title") {
          g_pack->title_font = InstallerFont::Create(drawer, data, e.width, e.height, 1.12f);
        }
        break;
      case InstallerPackType::kSpriteSheet: {
        uint32_t count;
        std::memcpy(&count, data, 4);
        const auto* sprites = reinterpret_cast<const InstallerSprite*>(data + 4);
        for (uint32_t s = 0; s < count; ++s) {
          g_pack->button_sprites[sprites[s].id] = sprites[s];
        }
        g_pack->buttons =
            drawer.CreateTexture(e.width, e.height, rex::ui::ImmediateTextureFilter::kLinear,
                                 false, data + 4 + count * sizeof(InstallerSprite));
        break;
      }
      case InstallerPackType::kText:
        if (std::string_view(e.name) == "strings") {
          ParseStrings(std::string_view(reinterpret_cast<const char*>(data), e.size));
        }
        break;
      default:
        break;
    }
  }
  if (!g_pack->body_font || !g_pack->title_font || !g_pack->buttons) {
    REXLOG_ERROR("Installer: the asset pack is missing fonts");
    g_pack.reset();
    return false;
  }
  return true;
}

void Unload() {
  g_pack.reset();
}

GuestTexture* Texture(std::string_view name) {
  if (!g_pack) return nullptr;
  auto it = g_pack->textures.find(name);
  return it == g_pack->textures.end() ? nullptr : it->second.get();
}

InstallerFont* BodyFont() {
  return g_pack ? g_pack->body_font.get() : nullptr;
}

InstallerFont* TitleFont() {
  return g_pack ? g_pack->title_font.get() : nullptr;
}

bool Button(uint32_t id, GuestTexture*& texture, ImVec2& uv_min, ImVec2& uv_max, ImVec2& size) {
  if (!g_pack) return false;
  auto it = g_pack->button_sprites.find(id);
  if (it == g_pack->button_sprites.end()) return false;
  const InstallerSprite& s = it->second;
  texture = g_pack->buttons.get();
  float w = float(texture->width), h = float(texture->height);
  uv_min = ImVec2(s.x / w, s.y / h);
  uv_max = ImVec2((s.x + s.w) / w, (s.y + s.h) / h);
  size = ImVec2(s.w, s.h);
  return true;
}

bool Sound(std::string_view name, const uint8_t*& data, size_t& size) {
  if (!g_pack) return false;
  auto it = g_pack->entries.find(name);
  if (it == g_pack->entries.end() || InstallerPackType(it->second->type) != InstallerPackType::kHca) {
    return false;
  }
  data = g_pack->data.data() + it->second->offset;
  size = it->second->size;
  return true;
}

}  // namespace InstallerAssets
