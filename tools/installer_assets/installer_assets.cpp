// Builds the installer's asset pack from the game's own files (sprites, fonts,
// button glyphs and sounds of Sonic the Fighters), so nothing from the game is
// kept in the repository. Run by the build when the game files are present.
//
// usage: installer_assets --media <_unpacked/media> --res <res/installer>
//                         --out <dir> [--bgm <cue name>]
//
// Output: <out>/installer_assets.bin (the pack, compressed with the Windows
// Compression API) and <out>/installer_assets.rc embedding it as RCDATA.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <compressapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <mspack.h>
#include <lzx.h>
}

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

#include "installer_pack.h"

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {

[[noreturn]] void Fail(const std::string& message) {
  throw std::runtime_error(message);
}

Bytes ReadFile(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    Fail("cannot read " + path.string());
  }
  return Bytes(std::istreambuf_iterator<char>(file), {});
}

void WriteFile(const fs::path& path, std::span<const uint8_t> data) {
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
  if (!file) {
    Fail("cannot write " + path.string());
  }
}

uint32_t BE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint16_t BE16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t LE32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint16_t LE16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
float BEFloat(const uint8_t* p) {
  uint32_t v = BE32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// ---------------------------------------------------------------------------
// The game's compressed files: 0x0FF512ED, version, checksum, then a flags word
// (block count in bits 6+, 32-bit sizes if bit 22 else 20-bit), the
// bit-packed uncompressed block sizes, and per block the LZX frames (window
// 2^17), each prefixed by its big-endian compressed size (0xFF: explicit
// uncompressed and compressed sizes).

struct MemoryFile {
  uint8_t* buffer;
  size_t size;
  size_t offset;
};

int MemoryRead(mspack_file* file, void* buffer, int bytes) {
  auto* m = reinterpret_cast<MemoryFile*>(file);
  size_t n = std::min(size_t(std::max(bytes, 0)), m->size - m->offset);
  std::memcpy(buffer, m->buffer + m->offset, n);
  m->offset += n;
  return int(n);
}
int MemoryWrite(mspack_file* file, void* buffer, int bytes) {
  auto* m = reinterpret_cast<MemoryFile*>(file);
  size_t n = std::min(size_t(std::max(bytes, 0)), m->size - m->offset);
  std::memcpy(m->buffer + m->offset, buffer, n);
  m->offset += n;
  return bytes;
}
void* MemoryAlloc(mspack_system*, size_t bytes) { return std::calloc(bytes, 1); }
void MemoryFree(void* p) { std::free(p); }
void MemoryCopy(void* src, void* dst, size_t bytes) { std::memmove(dst, src, bytes); }
void MemoryMessage(mspack_file*, const char*, ...) {}

Bytes Decompress(const Bytes& data) {
  if (data.size() < 16 || BE32(data.data()) != 0x0FF512ED) {
    return data;
  }
  uint32_t flags = BE32(&data[12]);
  uint32_t count = (flags >> 6) & 0xFFFF;
  uint32_t width = (flags & 0x00400000) ? 32 : 20;
  std::vector<uint32_t> sizes(count);
  size_t total = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t v = 0;
    for (uint32_t b = 0; b < width; ++b) {
      uint32_t bit = i * width + b;
      v = (v << 1) | ((data[16 + bit / 8] >> (7 - bit % 8)) & 1);
    }
    sizes[i] = v;
    total += v;
  }
  size_t p = 16 + ((size_t(count) * width + 31) / 32) * 4;
  Bytes out(total);
  size_t out_pos = 0;
  mspack_system sys = {};
  sys.read = MemoryRead;
  sys.write = MemoryWrite;
  sys.alloc = MemoryAlloc;
  sys.free = MemoryFree;
  sys.copy = MemoryCopy;
  sys.message = MemoryMessage;
  Bytes frames;
  for (uint32_t i = 0; i < count; ++i) {
    while (p + 2 <= data.size() && data[p] == 0 && data[p + 1] == 0) {
      p += 2;
    }
    frames.clear();
    int64_t remaining = sizes[i];
    while (remaining > 0) {
      if (p + 2 > data.size()) {
        Fail("truncated compressed file");
      }
      uint32_t compressed, uncompressed = 0x8000;
      if (data[p] == 0xFF) {
        uncompressed = BE16(&data[p + 1]);
        compressed = BE16(&data[p + 3]);
        p += 5;
      } else {
        compressed = BE16(&data[p]);
        p += 2;
      }
      if (p + compressed > data.size()) {
        Fail("truncated compressed frame");
      }
      frames.insert(frames.end(), data.begin() + p, data.begin() + p + compressed);
      p += compressed;
      remaining -= uncompressed;
    }
    MemoryFile src = {frames.data(), frames.size(), 0};
    MemoryFile dst = {out.data() + out_pos, sizes[i], 0};
    lzxd_stream* lzx = lzxd_init(&sys, reinterpret_cast<mspack_file*>(&src),
                                 reinterpret_cast<mspack_file*>(&dst), 17, 0, 0x8000,
                                 off_t(sizes[i]), 0);
    if (!lzx) {
      Fail("lzxd_init failed");
    }
    int result = lzxd_decompress(lzx, off_t(sizes[i]));
    lzxd_free(lzx);
    if (result != MSPACK_ERR_OK) {
      Fail("LZX block failed");
    }
    out_pos += sizes[i];
  }
  return out;
}

// SEGA FArc archive (uncompressed entries).
std::map<std::string, Bytes> ReadFArc(const Bytes& data) {
  if (data.size() < 12 || std::memcmp(data.data(), "FArc", 4) != 0) {
    Fail("not an FArc archive");
  }
  std::map<std::string, Bytes> files;
  size_t header_end = BE32(&data[4]) + 8;
  size_t p = 12;
  while (p < header_end) {
    std::string name(reinterpret_cast<const char*>(&data[p]));
    p += name.size() + 1;
    uint32_t offset = BE32(&data[p]), size = BE32(&data[p + 4]);
    p += 8;
    files[name] = Bytes(data.begin() + offset, data.begin() + offset + size);
  }
  return files;
}

// ---------------------------------------------------------------------------
// Images.

struct Image {
  int width = 0, height = 0;
  std::vector<uint8_t> rgba;

  Image() = default;
  Image(int w, int h) : width(w), height(h), rgba(size_t(w) * h * 4, 0) {}
  uint8_t* At(int x, int y) { return &rgba[(size_t(y) * width + x) * 4]; }
  const uint8_t* At(int x, int y) const { return &rgba[(size_t(y) * width + x) * 4]; }

  Image Crop(int x, int y, int w, int h) const {
    Image out(w, h);
    for (int j = 0; j < h; ++j) {
      for (int i = 0; i < w; ++i) {
        int sx = x + i, sy = y + j;
        if (sx >= 0 && sy >= 0 && sx < width && sy < height) {
          std::memcpy(out.At(i, j), At(sx, sy), 4);
        }
      }
    }
    return out;
  }
};

void DecodeDXT5Block(const uint8_t* block, uint8_t out[16][4]) {
  uint8_t a0 = block[0], a1 = block[1];
  uint8_t alpha[8] = {a0, a1};
  if (a0 > a1) {
    for (int i = 1; i < 7; ++i) alpha[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
  } else {
    for (int i = 1; i < 5; ++i) alpha[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5);
    alpha[6] = 0;
    alpha[7] = 255;
  }
  uint64_t alpha_bits = 0;
  for (int i = 0; i < 6; ++i) alpha_bits |= uint64_t(block[2 + i]) << (8 * i);
  uint16_t c0 = LE16(block + 8), c1 = LE16(block + 10);
  auto expand = [](uint16_t c, uint8_t rgb[3]) {
    rgb[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
    rgb[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    rgb[2] = uint8_t((c & 31) * 255 / 31);
  };
  uint8_t colors[4][3];
  expand(c0, colors[0]);
  expand(c1, colors[1]);
  for (int k = 0; k < 3; ++k) {
    colors[2][k] = uint8_t((2 * colors[0][k] + colors[1][k]) / 3);
    colors[3][k] = uint8_t((colors[0][k] + 2 * colors[1][k]) / 3);
  }
  uint32_t color_bits = LE32(block + 12);
  for (int i = 0; i < 16; ++i) {
    const uint8_t* c = colors[(color_bits >> (2 * i)) & 3];
    out[i][0] = c[0];
    out[i][1] = c[1];
    out[i][2] = c[2];
    out[i][3] = alpha[(alpha_bits >> (3 * i)) & 7];
  }
}

Image DecodeDDS(const Bytes& data) {
  if (data.size() < 128 || std::memcmp(data.data(), "DDS ", 4) != 0) {
    Fail("not a DDS file");
  }
  int height = int(LE32(&data[12])), width = int(LE32(&data[16]));
  uint32_t pf_flags = LE32(&data[80]);
  uint32_t fourcc = LE32(&data[84]);
  Image image(width, height);
  const uint8_t* pixels = data.data() + 128;
  if ((pf_flags & 4) && fourcc == 0x35545844) {  // DXT5
    int bw = (width + 3) / 4, bh = (height + 3) / 4;
    uint8_t block[16][4];
    for (int by = 0; by < bh; ++by) {
      for (int bx = 0; bx < bw; ++bx) {
        DecodeDXT5Block(pixels + (size_t(by) * bw + bx) * 16, block);
        for (int i = 0; i < 16; ++i) {
          int x = bx * 4 + i % 4, y = by * 4 + i / 4;
          if (x < width && y < height) std::memcpy(image.At(x, y), block[i], 4);
        }
      }
    }
  } else if ((pf_flags & 0x40) && LE32(&data[88]) == 32) {  // A8R8G8B8
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const uint8_t* s = pixels + (size_t(y) * width + x) * 4;
        uint8_t* d = image.At(x, y);
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = s[3];
      }
    }
  } else {
    Fail("unsupported DDS format");
  }
  return image;
}

Image LoadPNG(const fs::path& path) {
  Bytes data = ReadFile(path);
  int w, h, n;
  uint8_t* pixels = stbi_load_from_memory(data.data(), int(data.size()), &w, &h, &n, 4);
  if (!pixels) {
    Fail("cannot decode " + path.string());
  }
  Image image(w, h);
  std::memcpy(image.rgba.data(), pixels, image.rgba.size());
  stbi_image_free(pixels);
  return image;
}

Image Resize(const Image& src, int w, int h) {
  // Box filter (area average) with premultiplied alpha.
  Image out(w, h);
  double sx = double(src.width) / w, sy = double(src.height) / h;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      double x0 = x * sx, x1 = (x + 1) * sx, y0 = y * sy, y1 = (y + 1) * sy;
      double acc[4] = {}, area = 0;
      for (int j = int(y0); j < int(std::ceil(y1)) && j < src.height; ++j) {
        double wy = std::min(y1, j + 1.0) - std::max(y0, double(j));
        for (int i = int(x0); i < int(std::ceil(x1)) && i < src.width; ++i) {
          double wx = std::min(x1, i + 1.0) - std::max(x0, double(i));
          double wgt = wx * wy;
          const uint8_t* p = src.At(i, j);
          double a = p[3] / 255.0;
          acc[0] += p[0] * a * wgt;
          acc[1] += p[1] * a * wgt;
          acc[2] += p[2] * a * wgt;
          acc[3] += a * wgt;
          area += wgt;
        }
      }
      uint8_t* d = out.At(x, y);
      if (acc[3] > 1e-6) {
        for (int k = 0; k < 3; ++k) d[k] = uint8_t(std::clamp(acc[k] / acc[3], 0.0, 255.0));
      }
      d[3] = uint8_t(std::clamp(acc[3] / area * 255.0 + 0.5, 0.0, 255.0));
    }
  }
  return out;
}

// Sprite set: u32 texture count, sprite count, then offsets of 48-byte records
// (texture index, 3 unused, UV rectangle, pixel rectangle).
struct Sprite {
  uint32_t texture;
  int x, y, w, h;
};

struct SpriteSet {
  std::vector<Image> textures;
  std::vector<Sprite> sprites;

  Image Get(size_t index) const {
    const Sprite& s = sprites.at(index);
    return textures.at(s.texture).Crop(s.x, s.y, s.w, s.h);
  }
};

SpriteSet LoadSpriteSet(const fs::path& farc) {
  auto files = ReadFArc(Decompress(ReadFile(farc)));
  auto textures = ReadFArc(files.at("texture.farc"));
  SpriteSet set;
  // The sprite records index the compressed textures first, then the
  // uncompressed ones.
  std::vector<std::string> names;
  for (auto& [name, data] : textures) {
    if (name.find("nocomp") == std::string::npos) names.push_back(name);
  }
  for (auto& [name, data] : textures) {
    if (name.find("nocomp") != std::string::npos) names.push_back(name);
  }
  for (auto& name : names) {
    set.textures.push_back(DecodeDDS(textures.at(name)));
  }
  const Bytes& spr = files.at("sprite.bin");
  uint32_t count = BE32(&spr[4]);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* r = &spr[BE32(&spr[12 + i * 4])];
    set.sprites.push_back({BE32(r), int(BEFloat(r + 32)), int(BEFloat(r + 36)),
                           int(BEFloat(r + 40)), int(BEFloat(r + 44))});
  }
  return set;
}

// ---------------------------------------------------------------------------
// Fonts: FMH3 font map (little-endian) - per font a glyph cell size and
// stride, cells per row, then per glyph: UTF-16 code, flags, cell (row << 8 |
// column), ink x offset and width.

struct FontMapGlyph {
  uint32_t code;
  int cell_x, cell_y;
  int ink_x, ink_w;
};

struct FontMap {
  int cell_w, cell_h, stride_w, stride_h;
  std::map<uint32_t, FontMapGlyph> glyphs;
};

std::vector<FontMap> LoadFontMaps(const Bytes& data) {
  if (std::memcmp(data.data(), "FMH3", 4) != 0) {
    Fail("unexpected font map");
  }
  uint32_t count = LE32(&data[8]);
  uint32_t table = LE32(&data[12]);
  std::vector<FontMap> fonts;
  for (uint32_t f = 0; f < count; ++f) {
    const uint8_t* h = &data[LE32(&data[table + f * 4])];
    FontMap font;
    font.cell_w = h[4];
    font.cell_h = h[5];
    font.stride_w = h[6];
    font.stride_h = h[7];
    uint32_t glyph_count = LE32(h + 20), glyph_offset = LE32(h + 24);
    for (uint32_t i = 0; i < glyph_count; ++i) {
      const uint8_t* g = &data[glyph_offset + i * 8];
      uint16_t cell = LE16(g + 4);
      font.glyphs[LE16(g)] = {LE16(g), (cell & 0xFF) * font.stride_w, (cell >> 8) * font.stride_h,
                              g[6], g[7]};
    }
    fonts.push_back(std::move(font));
  }
  return fonts;
}

// A glyph's coverage bitmap in cell pixels, with the top-left of the line
// box at (origin_x, origin_y) inside the bitmap and the pen advance.
struct Coverage {
  int w = 0, h = 0;
  std::vector<float> a;
  int origin_x = 0, origin_y = 0;
  float advance = 0;
  float At(int x, int y) const {
    return (x < 0 || y < 0 || x >= w || y >= h) ? 0.0f : a[size_t(y) * w + x];
  }
};

Coverage GlyphCoverage(const FontMap& map, const Image& atlas, uint32_t code, float tracking) {
  const FontMapGlyph& g = map.glyphs.at(code);
  Coverage c;
  c.w = map.cell_w;
  c.h = map.cell_h;
  c.a.resize(size_t(c.w) * c.h);
  for (int y = 0; y < c.h; ++y) {
    for (int x = 0; x < c.w; ++x) {
      c.a[size_t(y) * c.w + x] = atlas.At(g.cell_x + x, g.cell_y + y)[3] / 255.0f;
    }
  }
  // Pen at the ink's left edge.
  c.origin_x = g.ink_x;
  c.origin_y = 0;
  c.advance = float(g.ink_w) + tracking;
  if (code == ' ') {
    // The game's space is a whole cell - a regular word space instead.
    c.advance = map.cell_h * 0.3f;
  } else if (code == 0x3000) {
    c.advance = float(map.cell_w);
  }
  return c;
}

// Ink bounds of a coverage bitmap.
bool InkBounds(const Coverage& c, int& x0, int& y0, int& x1, int& y1) {
  x0 = c.w;
  y0 = c.h;
  x1 = -1;
  y1 = -1;
  for (int y = 0; y < c.h; ++y) {
    for (int x = 0; x < c.w; ++x) {
      if (c.a[size_t(y) * c.w + x] > 0.25f) {
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
      }
    }
  }
  return x1 >= 0;
}

// Accented letters the game's fonts don't have, composed from the base letter
// and an accent glyph.
struct Composition {
  uint32_t code, base, accent;
  bool below = false, flip = false, mirror = false;
};

const Composition kCompositions[] = {
    {0xC0, 'A', '`'},   {0xC1, 'A', 0xB4},  {0xC2, 'A', '^'},   {0xC3, 'A', '~'},
    {0xC4, 'A', 0xA8},  {0xC5, 'A', 0xB0},  {0xC7, 'C', ',', true},
    {0xC8, 'E', '`'},   {0xC9, 'E', 0xB4},  {0xCA, 'E', '^'},   {0xCB, 'E', 0xA8},
    {0xCC, 'I', '`'},   {0xCD, 'I', 0xB4},  {0xCE, 'I', '^'},   {0xCF, 'I', 0xA8},
    {0xD1, 'N', '~'},   {0xD2, 'O', '`'},   {0xD3, 'O', 0xB4},  {0xD4, 'O', '^'},
    {0xD5, 'O', '~'},   {0xD6, 'O', 0xA8},  {0xD9, 'U', '`'},   {0xDA, 'U', 0xB4},
    {0xDB, 'U', '^'},   {0xDC, 'U', 0xA8},  {0xE0, 'a', '`'},   {0xE1, 'a', 0xB4},
    {0xE2, 'a', '^'},   {0xE3, 'a', '~'},   {0xE4, 'a', 0xA8},  {0xE5, 'a', 0xB0},
    {0xE7, 'c', ',', true},                 {0xE8, 'e', '`'},   {0xE9, 'e', 0xB4},
    {0xEA, 'e', '^'},   {0xEB, 'e', 0xA8},  {0xEC, 'i', '`'},   {0xED, 'i', 0xB4},
    {0xEE, 'i', '^'},   {0xEF, 'i', 0xA8},  {0xF1, 'n', '~'},   {0xF2, 'o', '`'},
    {0xF3, 'o', 0xB4},  {0xF4, 'o', '^'},   {0xF5, 'o', '~'},   {0xF6, 'o', 0xA8},
    {0xF9, 'u', '`'},   {0xFA, 'u', 0xB4},  {0xFB, 'u', '^'},   {0xFC, 'u', 0xA8},
    {0xA1, '!', 0, false, true},            {0xBF, '?', 0, false, true},
    // The game's fonts have a yen sign at the backslash (Shift JIS) - a
    // mirrored slash instead, for paths.
    {0x5C, '/', 0, false, false, true},
};

// Composes an accented glyph: the accent's ink, scaled, centred over (or
// under) the base letter's ink with a small gap. The bitmap grows upwards if
// the accent doesn't fit in the cell.
Coverage ComposeGlyph(const Coverage& base, const Coverage* accent, const Composition& comp) {
  if (comp.flip || comp.mirror) {
    Coverage out = base;
    int x0, y0, x1, y1;
    InkBounds(base, x0, y0, x1, y1);
    std::fill(out.a.begin(), out.a.end(), 0.0f);
    if (comp.mirror) {
      for (int y = 0; y < base.h; ++y) {
        for (int x = x0; x <= x1; ++x) {
          out.a[size_t(y) * out.w + (x0 + x1 - x)] = base.a[size_t(y) * base.w + x];
        }
      }
      return out;
    }
    // Rotate the ink by 180 degrees in place (upside-down ! and ?), keeping it
    // on the baseline: ink occupies the same rows reflected around its box.
    for (int y = y0; y <= y1; ++y) {
      for (int x = x0; x <= x1; ++x) {
        out.a[size_t(y0 + y1 - y) * out.w + (x0 + x1 - x)] = base.a[size_t(y) * base.w + x];
      }
    }
    return out;
  }
  int bx0, by0, bx1, by1, ax0, ay0, ax1, ay1;
  InkBounds(base, bx0, by0, bx1, by1);
  InkBounds(*accent, ax0, ay0, ax1, ay1);
  bool lower = comp.base >= 'a' && comp.base <= 'z';
  float scale = lower ? 0.85f : 0.75f;
  int aw = ax1 - ax0 + 1, ah = ay1 - ay0 + 1;
  int sw = std::max(1, int(std::lround(aw * scale))), sh = std::max(1, int(std::lround(ah * scale)));
  int gap = std::max(2, base.h / 20);
  float centre = (bx0 + bx1) * 0.5f;
  if (comp.base == 'i') {
    // Replace the dot of i.
    by0 = by0 + (by1 - by0) * 3 / 10;
  }
  int top = comp.below ? by1 + 1 : by0 - gap - sh;
  int extra = std::max(0, -top);
  Coverage out;
  out.w = base.w;
  out.h = base.h + extra + (comp.below ? sh + gap : 0);
  out.a.assign(size_t(out.w) * out.h, 0.0f);
  out.origin_x = base.origin_x;
  out.origin_y = base.origin_y + extra;
  out.advance = base.advance;
  for (int y = 0; y < base.h; ++y) {
    for (int x = 0; x < base.w; ++x) {
      float v = base.a[size_t(y) * base.w + x];
      if (comp.base == 'i' && y < by0) v = 0.0f;
      out.a[size_t(y + extra) * out.w + x] = v;
    }
  }
  int left = int(std::lround(centre - sw * 0.5f));
  if (comp.below) {
    // Cedilla: a comma under the letter, slightly right of centre.
    left += sw / 4;
  }
  for (int y = 0; y < sh; ++y) {
    for (int x = 0; x < sw; ++x) {
      // Bilinear sample of the accent's ink box.
      float fx = ax0 + (x + 0.5f) / scale - 0.5f, fy = ay0 + (y + 0.5f) / scale - 0.5f;
      int ix = int(std::floor(fx)), iy = int(std::floor(fy));
      float tx = fx - ix, ty = fy - iy;
      float v = (accent->At(ix, iy) * (1 - tx) + accent->At(ix + 1, iy) * tx) * (1 - ty) +
                (accent->At(ix, iy + 1) * (1 - tx) + accent->At(ix + 1, iy + 1) * tx) * ty;
      int ox = left + x, oy = top + extra + y;
      if (ox >= 0 && ox < out.w && oy >= 0 && oy < out.h) {
        float& d = out.a[size_t(oy) * out.w + ox];
        d = std::max(d, v);
      }
    }
  }
  return out;
}

// Scales a coverage bitmap (used to borrow the body font's accents for the
// title font).
Coverage ScaleCoverage(const Coverage& c, float scale) {
  Coverage out;
  out.w = std::max(1, int(std::lround(c.w * scale)));
  out.h = std::max(1, int(std::lround(c.h * scale)));
  out.a.resize(size_t(out.w) * out.h);
  for (int y = 0; y < out.h; ++y) {
    for (int x = 0; x < out.w; ++x) {
      float fx = (x + 0.5f) / scale - 0.5f, fy = (y + 0.5f) / scale - 0.5f;
      int ix = int(std::floor(fx)), iy = int(std::floor(fy));
      float tx = fx - ix, ty = fy - iy;
      out.a[size_t(y) * out.w + x] =
          (c.At(ix, iy) * (1 - tx) + c.At(ix + 1, iy) * tx) * (1 - ty) +
          (c.At(ix, iy + 1) * (1 - tx) + c.At(ix + 1, iy + 1) * tx) * ty;
    }
  }
  out.origin_x = int(std::lround(c.origin_x * scale));
  out.origin_y = int(std::lround(c.origin_y * scale));
  out.advance = c.advance * scale;
  return out;
}

// Signed distance field of a coverage bitmap: distance in pixels to the edge
// (the 0.5 coverage level, refined with the coverage of edge pixels),
// positive inside, encoded as 0.5 + distance / range.
constexpr int kSdfPadding = 5;
constexpr float kSdfRange = 8.0f;

std::vector<uint8_t> MakeSdf(const Coverage& c, int& w, int& h) {
  w = c.w + kSdfPadding * 2;
  h = c.h + kSdfPadding * 2;
  std::vector<uint8_t> out(size_t(w) * h);
  auto inside = [&](int x, int y) { return c.At(x, y) >= 0.5f; };
  const int radius = int(kSdfRange / 2) + 2;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      int cx = x - kSdfPadding, cy = y - kSdfPadding;
      bool in = inside(cx, cy);
      float best = float(radius);
      for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
          int nx = cx + dx, ny = cy + dy;
          if (inside(nx, ny) == in) continue;
          // The edge lies between this pixel and the neighbour: place it by
          // the neighbour's coverage (sub-pixel), approximately.
          float cov = c.At(nx, ny);
          float frac = in ? std::clamp(0.5f - cov, 0.0f, 0.5f) : std::clamp(cov - 0.5f, 0.0f, 0.5f);
          float dist = std::sqrt(float(dx * dx + dy * dy)) - (0.5f - frac);
          best = std::min(best, std::max(dist, 0.0f));
        }
      }
      float signed_distance = in ? best : -best;
      float v = 0.5f + signed_distance / kSdfRange;
      out[size_t(y) * w + x] = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
  }
  return out;
}

// Simple shelf packer.
struct Packer {
  int width, height = 0;
  int x = 0, y = 0, shelf = 0;
  explicit Packer(int w) : width(w) {}
  std::pair<int, int> Add(int w, int h) {
    if (x + w > width) {
      x = 0;
      y += shelf + 1;
      shelf = 0;
    }
    std::pair<int, int> pos{x, y};
    x += w + 1;
    shelf = std::max(shelf, h);
    height = std::max(height, y + h);
    return pos;
  }
};

// ---------------------------------------------------------------------------
// Output pack.

class PackWriter {
 public:
  void Add(const std::string& name, InstallerPackType type, uint32_t width, uint32_t height,
           std::span<const uint8_t> data, uint32_t extra = 0) {
    InstallerPackEntry entry = {};
    std::snprintf(entry.name, sizeof(entry.name), "%s", name.c_str());
    entry.type = uint32_t(type);
    entry.width = width;
    entry.height = height;
    entry.offset = uint32_t(blob_.size());
    entry.size = uint32_t(data.size());
    entry.extra = extra;
    entries_.push_back(entry);
    blob_.insert(blob_.end(), data.begin(), data.end());
    while (blob_.size() % 16) blob_.push_back(0);
  }

  void AddImage(const std::string& name, const Image& image) {
    Add(name, InstallerPackType::kImage, image.width, image.height, image.rgba);
  }

  Bytes Finish() const {
    InstallerPackHeader header = {};
    std::memcpy(header.magic, "STFI", 4);
    header.version = kInstallerPackVersion;
    header.entry_count = uint32_t(entries_.size());
    size_t data_start = sizeof(header) + entries_.size() * sizeof(InstallerPackEntry);
    Bytes out(data_start);
    std::memcpy(out.data(), &header, sizeof(header));
    for (size_t i = 0; i < entries_.size(); ++i) {
      InstallerPackEntry e = entries_[i];
      e.offset += uint32_t(data_start);
      std::memcpy(out.data() + sizeof(header) + i * sizeof(e), &e, sizeof(e));
    }
    out.insert(out.end(), blob_.begin(), blob_.end());
    return out;
  }

 private:
  std::vector<InstallerPackEntry> entries_;
  Bytes blob_;
};

// Builds one SDF font from a font map and its texture for the given code
// points (compositions where missing).
void AddFont(PackWriter& pack, const std::string& name, const FontMap& map, const Image& atlas,
             const std::set<uint32_t>& codes, float tracking, const FontMap* accent_map,
             const Image* accent_atlas) {
  struct Built {
    uint32_t code;
    Coverage coverage;
  };
  std::vector<Built> glyphs;
  for (uint32_t code : codes) {
    const Composition* comp = nullptr;
    for (const Composition& c : kCompositions) {
      if (c.code == code) comp = &c;
    }
    if (map.glyphs.count(code) && !(comp && comp->mirror)) {
      glyphs.push_back({code, GlyphCoverage(map, atlas, code, tracking)});
      continue;
    }
    if (!comp || !map.glyphs.count(comp->base)) {
      continue;
    }
    Coverage base = GlyphCoverage(map, atlas, comp->base, tracking);
    if (comp->flip || comp->mirror) {
      glyphs.push_back({code, ComposeGlyph(base, nullptr, *comp)});
      continue;
    }
    std::optional<Coverage> accent;
    if (map.glyphs.count(comp->accent)) {
      accent = GlyphCoverage(map, atlas, comp->accent, tracking);
    } else if (accent_map && accent_map->glyphs.count(comp->accent)) {
      accent = ScaleCoverage(GlyphCoverage(*accent_map, *accent_atlas, comp->accent, 0.0f),
                             float(map.cell_h) / float(accent_map->cell_h));
    }
    if (!accent) {
      continue;
    }
    glyphs.push_back({code, ComposeGlyph(base, &*accent, *comp)});
  }

  Packer packer(1024);
  struct Placed {
    InstallerFontGlyph glyph;
    std::vector<uint8_t> sdf;
    int w, h;
  };
  std::vector<Placed> placed;
  for (Built& b : glyphs) {
    Placed p;
    p.sdf = MakeSdf(b.coverage, p.w, p.h);
    auto [x, y] = packer.Add(p.w, p.h);
    p.glyph.code = b.code;
    p.glyph.x = uint16_t(x);
    p.glyph.y = uint16_t(y);
    p.glyph.w = uint16_t(p.w);
    p.glyph.h = uint16_t(p.h);
    // Offsets of the SDF box from the pen position at the top of the line.
    p.glyph.offset_x = float(-b.coverage.origin_x - kSdfPadding);
    p.glyph.offset_y = float(-b.coverage.origin_y - kSdfPadding);
    p.glyph.advance = b.coverage.advance;
    placed.push_back(std::move(p));
  }
  int aw = packer.width, ah = (packer.height + 3) & ~3;
  std::vector<uint8_t> sdf_atlas(size_t(aw) * ah, 0);
  for (Placed& p : placed) {
    for (int y = 0; y < p.h; ++y) {
      std::memcpy(&sdf_atlas[size_t(p.glyph.y + y) * aw + p.glyph.x], &p.sdf[size_t(y) * p.w], p.w);
    }
  }
  InstallerFontHeader header = {};
  header.glyph_count = uint32_t(placed.size());
  header.line_height = float(map.cell_h);
  header.sdf_range = kSdfRange;
  Bytes data(sizeof(header));
  std::memcpy(data.data(), &header, sizeof(header));
  for (Placed& p : placed) {
    const uint8_t* g = reinterpret_cast<const uint8_t*>(&p.glyph);
    data.insert(data.end(), g, g + sizeof(p.glyph));
  }
  data.insert(data.end(), sdf_atlas.begin(), sdf_atlas.end());
  pack.Add(name, InstallerPackType::kFont, aw, ah, data);
  std::printf("  font %s: %zu glyphs, %dx%d atlas\n", name.c_str(), placed.size(), aw, ah);
}

// Controller button glyphs of the body font (colour), as a sprite sheet: two
// cell halves for the wide ones.
void AddButtons(PackWriter& pack, const FontMap& map, const Image& atlas) {
  struct Button {
    uint32_t id;
    std::vector<uint32_t> cells;
  };
  const Button buttons[] = {
      {kButtonXboxA, {0xE001}},    {kButtonXboxB, {0xE000}},          {kButtonXboxX, {0xE003}},
      {kButtonXboxY, {0xE002}},    {kButtonXboxStart, {0xE004, 0xE005}},
      {kButtonXboxBack, {0xE006, 0xE007}},    {kButtonXboxLB, {0xE008, 0xE009}},
      {kButtonXboxLT, {0xE00A, 0xE00B}},      {kButtonXboxRB, {0xE00C, 0xE00D}},
      {kButtonXboxRT, {0xE00E, 0xE00F}},      {kButtonDpad, {0xE014}},
      {kButtonPSCircle, {0xE015}}, {kButtonPSCross, {0xE016}},        {kButtonPSTriangle, {0xE017}},
      {kButtonPSSquare, {0xE018}},
  };
  Packer packer(512);
  struct Placed {
    InstallerSprite sprite;
    Image image;
  };
  std::vector<Placed> placed;
  for (const Button& b : buttons) {
    int total = 0;
    for (uint32_t c : b.cells) total += map.glyphs.at(c).ink_w;
    Image image(total, map.cell_h);
    int x = 0;
    for (uint32_t c : b.cells) {
      const FontMapGlyph& g = map.glyphs.at(c);
      Image part = atlas.Crop(g.cell_x + g.ink_x, g.cell_y, g.ink_w, map.cell_h);
      for (int y = 0; y < part.height; ++y) {
        for (int i = 0; i < part.width; ++i) std::memcpy(image.At(x + i, y), part.At(i, y), 4);
      }
      x += g.ink_w;
    }
    // Trim to the ink rows.
    int y0 = image.height, y1 = -1;
    for (int y = 0; y < image.height; ++y) {
      for (int i = 0; i < image.width; ++i) {
        if (image.At(i, y)[3] > 8) {
          y0 = std::min(y0, y);
          y1 = std::max(y1, y);
        }
      }
    }
    if (y1 >= y0) image = image.Crop(0, y0, image.width, y1 - y0 + 1);
    auto [px, py] = packer.Add(image.width, image.height);
    placed.push_back({{b.id, uint16_t(px), uint16_t(py), uint16_t(image.width),
                       uint16_t(image.height)},
                      std::move(image)});
  }
  Image sheet(packer.width, packer.height);
  for (Placed& p : placed) {
    for (int y = 0; y < p.image.height; ++y) {
      for (int x = 0; x < p.image.width; ++x) {
        std::memcpy(sheet.At(p.sprite.x + x, p.sprite.y + y), p.image.At(x, y), 4);
      }
    }
  }
  Bytes data;
  uint32_t count = uint32_t(placed.size());
  data.insert(data.end(), reinterpret_cast<uint8_t*>(&count), reinterpret_cast<uint8_t*>(&count) + 4);
  for (Placed& p : placed) {
    const uint8_t* s = reinterpret_cast<const uint8_t*>(&p.sprite);
    data.insert(data.end(), s, s + sizeof(p.sprite));
  }
  data.insert(data.end(), sheet.rgba.begin(), sheet.rgba.end());
  pack.Add("buttons", InstallerPackType::kSpriteSheet, sheet.width, sheet.height, data);
}

// ---------------------------------------------------------------------------
// CRI ACB (@UTF tables) and AFS2 archives.

struct UtfValue {
  enum class Kind { kNone, kInt, kFloat, kString, kData } kind = Kind::kNone;
  uint64_t i = 0;
  double f = 0;
  std::string s;
  size_t data_offset = 0, data_size = 0;
};
using UtfRow = std::map<std::string, UtfValue>;

std::vector<UtfRow> ReadUtf(const Bytes& data, size_t offset) {
  if (std::memcmp(&data[offset], "@UTF", 4) != 0) {
    Fail("not a @UTF table");
  }
  size_t base = offset + 8;
  uint32_t rows_offset = BE16(&data[base + 2]);
  uint32_t strings_offset = BE32(&data[base + 4]);
  uint32_t data_offset = BE32(&data[base + 8]);
  uint16_t column_count = BE16(&data[base + 16]);
  uint16_t row_width = BE16(&data[base + 18]);
  uint32_t row_count = BE32(&data[base + 20]);
  const char* strings = reinterpret_cast<const char*>(&data[base + strings_offset]);
  size_t blob = base + data_offset;
  auto read_value = [&](size_t& p, uint8_t type) {
    UtfValue v;
    switch (type) {
      case 0: case 1: v.kind = UtfValue::Kind::kInt; v.i = data[p]; p += 1; break;
      case 2: case 3: v.kind = UtfValue::Kind::kInt; v.i = BE16(&data[p]); p += 2; break;
      case 4: case 5: v.kind = UtfValue::Kind::kInt; v.i = BE32(&data[p]); p += 4; break;
      case 6: case 7:
        v.kind = UtfValue::Kind::kInt;
        v.i = (uint64_t(BE32(&data[p])) << 32) | BE32(&data[p + 4]);
        p += 8;
        break;
      case 8: v.kind = UtfValue::Kind::kFloat; v.f = BEFloat(&data[p]); p += 4; break;
      case 0xA: v.kind = UtfValue::Kind::kString; v.s = strings + BE32(&data[p]); p += 4; break;
      case 0xB:
        v.kind = UtfValue::Kind::kData;
        v.data_offset = blob + BE32(&data[p]);
        v.data_size = BE32(&data[p + 4]);
        p += 8;
        break;
      default: Fail("unsupported @UTF type");
    }
    return v;
  };
  struct Column {
    std::string name;
    uint8_t storage, type;
    UtfValue constant;
  };
  std::vector<Column> columns;
  size_t p = base + 24;
  for (uint16_t c = 0; c < column_count; ++c) {
    Column col;
    uint8_t flags = data[p++];
    col.storage = flags & 0xF0;
    col.type = flags & 0x0F;
    col.name = strings + BE32(&data[p]);
    p += 4;
    if (col.storage == 0x30) col.constant = read_value(p, col.type);
    columns.push_back(col);
  }
  std::vector<UtfRow> rows(row_count);
  for (uint32_t r = 0; r < row_count; ++r) {
    size_t q = base + rows_offset + size_t(r) * row_width;
    for (Column& col : columns) {
      if (col.storage == 0x50) rows[r][col.name] = read_value(q, col.type);
      else if (col.storage == 0x30) rows[r][col.name] = col.constant;
    }
  }
  return rows;
}

std::map<uint32_t, std::pair<size_t, size_t>> ReadAfs2(const uint8_t* data, size_t size) {
  if (size < 16 || std::memcmp(data, "AFS2", 4) != 0) {
    Fail("not an AFS2 archive");
  }
  uint32_t offset_size = data[5], id_size = data[6];
  uint32_t count = LE32(data + 8), align = LE32(data + 12) & 0xFFFF;
  auto read = [&](size_t p, uint32_t bytes) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < bytes; ++i) v |= uint32_t(data[p + i]) << (8 * i);
    return v;
  };
  std::map<uint32_t, std::pair<size_t, size_t>> entries;
  size_t offsets = 16 + size_t(count) * id_size;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t id = read(16 + i * id_size, id_size);
    size_t start = read(offsets + i * offset_size, offset_size);
    size_t end = read(offsets + (i + 1) * offset_size, offset_size);
    start = (start + align - 1) / align * align;
    entries[id] = {start, end - start};
  }
  return entries;
}

// Extracts the HCA data of the given cues.
std::map<std::string, Bytes> ExtractCues(const fs::path& acb_path, const fs::path& awb_path,
                                         const std::vector<std::string>& cues) {
  Bytes acb = Decompress(ReadFile(acb_path));
  UtfRow header = ReadUtf(acb, 0).at(0);
  auto table = [&](const char* name) { return ReadUtf(acb, header.at(name).data_offset); };
  auto cue_table = table("CueTable");
  auto cue_names = table("CueNameTable");
  auto synths = table("SynthTable");
  auto waveforms = table("WaveformTable");
  const UtfValue& memory_awb = header.at("AwbFile");
  auto memory = ReadAfs2(&acb[memory_awb.data_offset], memory_awb.data_size);
  Bytes stream_awb;
  std::map<uint32_t, std::pair<size_t, size_t>> streams;

  std::map<std::string, Bytes> out;
  for (const UtfRow& name_row : cue_names) {
    const std::string& name = name_row.at("CueName").s;
    if (std::find(cues.begin(), cues.end(), name) == cues.end()) continue;
    const UtfRow& cue = cue_table.at(name_row.at("CueIndex").i);
    if (cue.at("ReferenceType").i != 2) Fail("unexpected cue reference for " + name);
    // Follow the synth's first reference down to a waveform (1: waveform,
    // 2: nested synth).
    uint64_t synth = cue.at("ReferenceIndex").i;
    uint16_t wave_index = 0;
    for (int depth = 0;; ++depth) {
      const UtfValue& refs = synths.at(synth).at("ReferenceItems");
      if (refs.data_size < 4 || depth > 8) Fail("unexpected synth for " + name);
      uint16_t type = BE16(&acb[refs.data_offset]), index = BE16(&acb[refs.data_offset + 2]);
      if (type == 1) {
        wave_index = index;
        break;
      }
      if (type != 2) Fail("unexpected synth reference for " + name);
      synth = index;
    }
    const UtfRow& wave = waveforms.at(wave_index);
    uint32_t id = uint32_t(wave.count("Id") ? wave.at("Id").i : wave.at("MemoryAwbId").i);
    if (wave.at("Streaming").i) {
      if (stream_awb.empty()) {
        stream_awb = ReadFile(awb_path);
        streams = ReadAfs2(stream_awb.data(), stream_awb.size());
      }
      auto [start, size] = streams.at(id);
      out[name] = Bytes(stream_awb.begin() + start, stream_awb.begin() + start + size);
    } else {
      auto [start, size] = memory.at(id);
      const uint8_t* base = &acb[memory_awb.data_offset];
      out[name] = Bytes(base + start, base + start + size);
    }
  }
  for (const std::string& name : cues) {
    if (!out.count(name)) Fail("cue not found: " + name);
  }
  return out;
}

// ---------------------------------------------------------------------------

// UTF-8 code points of a text (the installer's strings).
std::set<uint32_t> CodePoints(std::string_view text) {
  std::set<uint32_t> out;
  for (size_t i = 0; i < text.size();) {
    uint8_t c = uint8_t(text[i]);
    uint32_t cp;
    int n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
    else { cp = c & 0x07; n = 4; }
    for (int k = 1; k < n && i + k < text.size(); ++k) cp = (cp << 6) | (uint8_t(text[i + k]) & 0x3F);
    i += n;
    if (cp >= 0x20) out.insert(cp);
  }
  return out;
}

// Character renders (transparent, prepared by key_characters.py), cropped to
// the character and fit into a square.
Image CharacterImage(const Image& source, int size) {
  int x0 = source.width, y0 = source.height, x1 = -1, y1 = -1;
  for (int y = 0; y < source.height; ++y) {
    for (int x = 0; x < source.width; ++x) {
      if (source.At(x, y)[3] > 8) {
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
      }
    }
  }
  if (x1 < 0) Fail("empty character render");
  Image cut = source.Crop(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
  float scale = std::min(float(size) / cut.width, float(size) / cut.height);
  Image scaled = Resize(cut, std::max(1, int(std::lround(cut.width * scale))),
                        std::max(1, int(std::lround(cut.height * scale))));
  Image out(size, size);
  int ox = (size - scaled.width) / 2, oy = (size - scaled.height) / 2;
  for (int y = 0; y < scaled.height; ++y) {
    for (int x = 0; x < scaled.width; ++x) std::memcpy(out.At(ox + x, oy + y), scaled.At(x, y), 4);
  }
  return out;
}

Bytes CompressPack(const Bytes& data) {
  COMPRESSOR_HANDLE compressor = nullptr;
  if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &compressor)) {
    Fail("CreateCompressor failed");
  }
  SIZE_T size = 0;
  Compress(compressor, data.data(), data.size(), nullptr, 0, &size);
  Bytes out(size);
  if (!Compress(compressor, data.data(), data.size(), out.data(), out.size(), &size)) {
    CloseCompressor(compressor);
    Fail("Compress failed");
  }
  CloseCompressor(compressor);
  out.resize(size);
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    fs::path media, res, out, raw_path, logo;
    std::string bgm = "bgm00";
    for (int i = 1; i + 1 < argc; i += 2) {
      std::string_view arg = argv[i];
      if (arg == "--media") media = argv[i + 1];
      else if (arg == "--res") res = argv[i + 1];
      else if (arg == "--out") out = argv[i + 1];
      else if (arg == "--bgm") bgm = argv[i + 1];
      else if (arg == "--raw") raw_path = argv[i + 1];
      else if (arg == "--logo") logo = argv[i + 1];
    }
    if (media.empty() || res.empty() || out.empty()) {
      std::fprintf(stderr, "usage: installer_assets --media <dir> --res <dir> --out <dir> "
                           "[--logo <png>] [--bgm <cue>] [--raw <file>]\n");
      return 2;
    }
    fs::create_directories(out);
    PackWriter pack;

    // Strings (and so the glyphs the fonts need).
    Bytes strings = ReadFile(res / "strings.txt");
    pack.Add("strings", InstallerPackType::kText, 0, 0, strings);
    std::set<uint32_t> codes = CodePoints(std::string_view(reinterpret_cast<char*>(strings.data()),
                                                           strings.size()));
    for (uint32_t c = 0x20; c < 0x7F; ++c) codes.insert(c);

    // Fonts.
    auto fonts = ReadFArc(Decompress(ReadFile(media / "rom" / "fontmap.farc")));
    auto maps = LoadFontMaps(fonts.at("fontmap.bin"));
    SpriteSet font_textures = LoadSpriteSet(media / "rom" / "sprite" / "n_fnt.farc");
    // Textures: 0 - debug font, 1 - title font, 2 - body font.
    const Image& body_atlas = font_textures.textures.at(2);
    const Image& title_atlas = font_textures.textures.at(1);
    AddFont(pack, "font_body", maps.at(0), body_atlas, codes, 2.0f, nullptr, nullptr);
    std::set<uint32_t> title_codes;
    for (uint32_t c : codes) {
      if (c < 0x250) title_codes.insert(c);
    }
    AddFont(pack, "font_title", maps.at(1), title_atlas, title_codes, 3.0f, &maps.at(0), &body_atlas);
    AddButtons(pack, maps.at(0), body_atlas);

    // Sprites.
    SpriteSet stf = LoadSpriteSet(media / "rom" / "sprite" / "n_stf.farc");
    SpriteSet cmn = LoadSpriteSet(media / "rom" / "sprite" / "n_cmn.farc");
    pack.AddImage("logo", stf.Get(25));
    pack.AddImage("sonic_icon", stf.Get(10));
    pack.AddImage("sega", stf.Get(2));
    pack.AddImage("loading_arc", cmn.Get(74));
    pack.AddImage("ring_glow", cmn.Get(71));
    pack.AddImage("glow_dot", cmn.Get(15));

    // The project's logo.
    if (!logo.empty()) {
      Image image = LoadPNG(logo);
      int w = 768, h = int(std::lround(double(image.height) * w / image.width));
      pack.AddImage("project_logo", Resize(image, w, h));
    }

    // UnleashedRecomp's keyboard and mouse icons (kept in the repository).
    pack.AddImage("kbm", LoadPNG(res / "kbm.png"));

    // Character renders, shown in turn.
    const char* characters[] = {"sonic", "tails", "knuckles", "amy",         "espio",
                                "fang",  "bean",  "bark",     "metal_sonic", "robotnik"};
    for (size_t i = 0; i < std::size(characters); ++i) {
      fs::path file = res / "characters" / (std::string(characters[i]) + ".png");
      pack.AddImage("character_" + std::to_string(i), CharacterImage(LoadPNG(file), 1024));
    }

    // Sounds.
    std::vector<std::string> cues = {"sy_vf4ps2_select", "sy_vf4ps2_success", "sy_vf4ps2_fail",
                                     "ring_2", bgm};
    auto sounds = ExtractCues(media / "rom" / "sound" / "stf_all.acb",
                              media / "rom" / "sound" / "stf_all.awb", cues);
    pack.Add("sound_cursor", InstallerPackType::kHca, 0, 0, sounds.at("sy_vf4ps2_select"));
    pack.Add("sound_decide", InstallerPackType::kHca, 0, 0, sounds.at("sy_vf4ps2_success"));
    pack.Add("sound_cancel", InstallerPackType::kHca, 0, 0, sounds.at("sy_vf4ps2_fail"));
    pack.Add("sound_ring", InstallerPackType::kHca, 0, 0, sounds.at("ring_2"));
    pack.Add("music", InstallerPackType::kHca, 0, 0, sounds.at(bgm));

    Bytes raw = pack.Finish();
    if (!raw_path.empty()) {
      // Uncompressed copy, for inspecting the pack.
      WriteFile(raw_path, raw);
    }
    Bytes compressed = CompressPack(raw);
    InstallerPackBlobHeader blob = {};
    std::memcpy(blob.magic, "STFZ", 4);
    blob.uncompressed_size = uint32_t(raw.size());
    blob.compressed_size = uint32_t(compressed.size());
    Bytes file(sizeof(blob));
    std::memcpy(file.data(), &blob, sizeof(blob));
    file.insert(file.end(), compressed.begin(), compressed.end());
    WriteFile(out / "installer_assets.bin", file);
    std::string rc = "STF_INSTALLER_ASSETS RCDATA \"installer_assets.bin\"\n";
    std::ofstream(out / "installer_assets.rc") << rc;
    std::printf("installer assets: %zu bytes (%zu compressed)\n", raw.size(), compressed.size());
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "installer_assets: %s\n", e.what());
    return 1;
  }
}
