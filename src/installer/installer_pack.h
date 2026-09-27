// Layout of the installer's asset pack, built from the game's files by
// tools/installer_assets and embedded as a Windows resource.
#pragma once

#include <cstdint>

// Resource blob: this header, then the pack compressed with the Windows
// Compression API (LZMS).
struct InstallerPackBlobHeader {
  char magic[4];  // "STFZ"
  uint32_t uncompressed_size;
  uint32_t compressed_size;
  uint32_t reserved;
};

constexpr uint32_t kInstallerPackVersion = 1;

struct InstallerPackHeader {
  char magic[4];  // "STFI"
  uint32_t version;
  uint32_t entry_count;
  uint32_t reserved;
};

enum class InstallerPackType : uint32_t {
  // R8G8B8A8, width x height.
  kImage = 1,
  // InstallerFontHeader, InstallerFontGlyph[glyph_count], then the R8 signed
  // distance field atlas (width x height).
  kFont = 2,
  // CRI HCA audio.
  kHca = 3,
  // UTF-8 text.
  kText = 4,
  // uint32_t count, InstallerSprite[count], then the R8G8B8A8 sheet.
  kSpriteSheet = 5,
};

struct InstallerPackEntry {
  char name[48];
  uint32_t type;
  uint32_t width;
  uint32_t height;
  uint32_t offset;  // From the start of the pack.
  uint32_t size;
  uint32_t extra;
};

struct InstallerFontHeader {
  uint32_t glyph_count;
  // Height of a line in atlas pixels (the font's size).
  float line_height;
  // Signed distance range in atlas pixels (0.5 is the edge).
  float sdf_range;
  uint32_t reserved;
};

struct InstallerFontGlyph {
  uint32_t code;
  uint16_t x, y, w, h;
  // Offset of the glyph box from the pen at the top of the line, and the pen
  // advance, in atlas pixels.
  float offset_x, offset_y;
  float advance;
};

struct InstallerSprite {
  uint32_t id;
  uint16_t x, y, w, h;
};

// Button sprite ids (the game's own controller glyphs).
enum : uint32_t {
  kButtonXboxA = 1,
  kButtonXboxB,
  kButtonXboxX,
  kButtonXboxY,
  kButtonXboxStart,
  kButtonXboxBack,
  kButtonXboxLB,
  kButtonXboxLT,
  kButtonXboxRB,
  kButtonXboxRT,
  kButtonDpad,
  kButtonPSCross,
  kButtonPSCircle,
  kButtonPSSquare,
  kButtonPSTriangle,
};
