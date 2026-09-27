#include "installer.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <set>
#include <vector>

namespace {

// The Xbox 360 World release of Sonic the Fighters.
constexpr uint32_t kTitleId = 0x5841129E;
constexpr uint32_t kContentType = 0x000D0000;
constexpr uint32_t kMediaId = 0x1BCD667A;
constexpr const wchar_t* kPackageName = L"17DB4B597093061B64BFC3E0BCB000BFF14EACBB58";
constexpr uint64_t kPackageSize = 101867520;
constexpr std::array<uint8_t, 32> kPackageSha256 = {
    0x6A, 0xC5, 0xA4, 0xB1, 0xD4, 0x73, 0x19, 0x1D, 0x8C, 0x0F, 0xA5, 0x00, 0x80, 0x33, 0xA3, 0xA5,
    0xF9, 0x6A, 0xF2, 0xF6, 0xBC, 0xB8, 0xD5, 0xE5, 0x4B, 0x3A, 0x62, 0x13, 0xA5, 0x1F, 0x31, 0x8E};

uint32_t BE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Reads the STFS header's identity.
bool ReadPackageHeader(const std::filesystem::path& path, uint32_t& title, uint32_t& type,
                       uint32_t& media, uint64_t& size) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) return false;
  size = std::filesystem::file_size(path, error);
  if (error || size < 0xA000) return false;
  std::ifstream file(path, std::ios::binary);
  std::array<uint8_t, 0x400> header;
  if (!file.read(reinterpret_cast<char*>(header.data()), header.size())) return false;
  if (std::memcmp(header.data(), "LIVE", 4) != 0 && std::memcmp(header.data(), "PIRS", 4) != 0 &&
      std::memcmp(header.data(), "CON ", 4) != 0) {
    return false;
  }
  type = BE32(&header[0x344]);
  media = BE32(&header[0x354]);
  title = BE32(&header[0x360]);
  return true;
}

class Hasher {
 public:
  explicit Hasher(LPCWSTR algorithm) {
    if (BCryptOpenAlgorithmProvider(&algorithm_, algorithm, nullptr, 0) >= 0) {
      DWORD length = 0, result = 0;
      BCryptGetProperty(algorithm_, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&length),
                        sizeof(length), &result, 0);
      length_ = length;
      BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0);
    }
  }
  ~Hasher() {
    if (hash_) BCryptDestroyHash(hash_);
    if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
  }
  bool Valid() const { return hash_ != nullptr; }
  void Update(const void* data, size_t size) {
    BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(data)), ULONG(size), 0);
  }
  std::vector<uint8_t> Finish() {
    std::vector<uint8_t> out(length_);
    BCryptFinishHash(hash_, out.data(), ULONG(out.size()), 0);
    BCryptDestroyHash(hash_);
    hash_ = nullptr;
    return out;
  }

 private:
  BCRYPT_ALG_HANDLE algorithm_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
  size_t length_ = 0;
};

// Read-only STFS package reader, verifying each block against the hash table.
class Stfs {
 public:
  explicit Stfs(std::vector<uint8_t> data) : data_(std::move(data)) {}

  struct Entry {
    std::filesystem::path path;
    bool directory;
    uint32_t size;
    uint32_t start;
  };

  bool ReadEntries(std::vector<Entry>& entries, std::string& error) {
    if ((data_[0x37B] & 1) == 0) {
      error = "unsupported STFS layout";
      return false;
    }
    base_offset_ = (uint64_t(BE32(&data_[0x340])) + 4095) & ~uint64_t(4095);
    total_blocks_ = (uint32_t(data_[0x395]) << 24) | (uint32_t(data_[0x396]) << 16) |
                    (uint32_t(data_[0x397]) << 8) | data_[0x398];
    uint32_t table_block = data_[0x37E] | (data_[0x37F] << 8) | (data_[0x380] << 16);
    uint32_t table_count = data_[0x37C] | (data_[0x37D] << 8);
    std::set<uint32_t> seen;
    for (uint32_t t = 0; t < table_count; ++t) {
      if (!seen.insert(table_block).second) {
        error = "STFS directory chain cycle";
        return false;
      }
      const uint8_t* block;
      uint32_t next;
      if (!Block(table_block, block, next, error)) return false;
      table_block = next;
      for (uint32_t offset = 0; offset < 4096; offset += 64) {
        const uint8_t* raw = block + offset;
        if (raw[0] == 0) break;
        uint32_t name_length = raw[40] & 63;
        std::string name(reinterpret_cast<const char*>(raw), name_length);
        if (name.empty() || name == "." || name == ".." ||
            name.find_first_of("/\\:") != std::string::npos) {
          error = "unsafe STFS entry name";
          return false;
        }
        uint32_t parent = (raw[50] << 8) | raw[51];
        std::filesystem::path path = std::filesystem::u8path(name);
        if (parent != 0xFFFF) {
          if (parent >= entries.size() || !entries[parent].directory) {
            error = "invalid STFS directory parent";
            return false;
          }
          path = entries[parent].path / path;
        }
        entries.push_back({path, (raw[40] & 0x80) != 0, BE32(raw + 52),
                           uint32_t(raw[47] | (raw[48] << 8) | (raw[49] << 16))});
      }
    }
    return true;
  }

  bool ReadFile(const Entry& entry, std::vector<uint8_t>& out, std::string& error) {
    out.clear();
    out.reserve(entry.size);
    uint32_t remaining = entry.size, index = entry.start;
    std::set<uint32_t> seen;
    while (remaining > 0) {
      if (!seen.insert(index).second) {
        error = "STFS file chain cycle";
        return false;
      }
      const uint8_t* block;
      uint32_t next;
      if (!Block(index, block, next, error)) return false;
      uint32_t count = std::min<uint32_t>(4096, remaining);
      out.insert(out.end(), block, block + count);
      remaining -= count;
      index = next;
    }
    return true;
  }

 private:
  bool Block(uint32_t index, const uint8_t*& content, uint32_t& next, std::string& error) {
    if (index >= total_blocks_) {
      error = "STFS block index out of range";
      return false;
    }
    uint64_t physical = index, level = 170;
    for (int i = 0; i < 3; ++i) {
      physical += (index + level) / level;
      if (index < level) break;
      level *= 170;
    }
    uint64_t offset = base_offset_ + physical * 4096;
    if (offset + 4096 > data_.size()) {
      error = "truncated STFS block";
      return false;
    }
    content = &data_[offset];
    uint64_t table = index < 170 ? 0 : (index / 170) * 171ull + index / 28900 + 1 +
                                           (index >= 28900 ? 1 : 0);
    uint64_t record = base_offset_ + table * 4096 + (index % 170) * 24ull;
    Hasher sha1(BCRYPT_SHA1_ALGORITHM);
    sha1.Update(content, 4096);
    std::vector<uint8_t> hash = sha1.Finish();
    if (std::memcmp(hash.data(), &data_[record], 20) != 0) {
      error = "STFS block " + std::to_string(index) + " failed verification";
      return false;
    }
    next = (uint32_t(data_[record + 21]) << 16) | (uint32_t(data_[record + 22]) << 8) |
           data_[record + 23];
    return true;
  }

  std::vector<uint8_t> data_;
  uint64_t base_offset_ = 0;
  uint32_t total_blocks_ = 0;
};

bool IsSupportedPackage(const std::filesystem::path& path) {
  uint32_t title, type, media;
  uint64_t size;
  return ReadPackageHeader(path, title, type, media, size) && title == kTitleId &&
         type == kContentType;
}

}  // namespace

bool Installer::parseGame(const std::filesystem::path& path, std::filesystem::path& package) {
  std::error_code error;
  if (std::filesystem::is_regular_file(path, error)) {
    if (IsSupportedPackage(path)) {
      package = path;
      return true;
    }
    return false;
  }
  if (!std::filesystem::is_directory(path, error)) return false;
  // The package inside the folder (the Xbox 360 content layout
  // 5841129E\000D0000\<package>, from any level).
  const std::filesystem::path candidates[] = {
      path / kPackageName,
      path / L"000D0000" / kPackageName,
      path / L"5841129E" / L"000D0000" / kPackageName,
  };
  for (const auto& candidate : candidates) {
    if (IsSupportedPackage(candidate)) {
      package = candidate;
      return true;
    }
  }
  // Any package of the title in the folder tree (a few levels deep).
  for (auto it = std::filesystem::recursive_directory_iterator(
           path, std::filesystem::directory_options::skip_permission_denied, error);
       !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
    if (it.depth() > 4) {
      it.disable_recursion_pending();
      continue;
    }
    if (it->is_regular_file(error) && IsSupportedPackage(it->path())) {
      package = it->path();
      return true;
    }
  }
  return false;
}

bool Installer::parseSources(const std::filesystem::path& package, Journal& journal,
                             Sources& sources) {
  uint32_t title, type, media;
  uint64_t size;
  if (!ReadPackageHeader(package, title, type, media, size) || title != kTitleId ||
      type != kContentType || media != kMediaId || size != kPackageSize) {
    journal.lastErrorMessage = "Unsupported Sonic the Fighters package.";
    return false;
  }
  sources.package = package;
  // The extracted data is about the size of the package.
  sources.totalSize = size;
  return true;
}

bool Installer::checkInstall(const std::filesystem::path& installPath) {
  std::error_code error;
  return std::filesystem::is_regular_file(installPath / "default.xex", error);
}

bool Installer::install(const Sources& sources, const std::filesystem::path& installPath,
                        Journal& journal, std::function<bool()> progressCallback) {
  // Progress: hashing the package, then writing the files (by bytes).
  std::error_code error;
  uint64_t packageSize = std::filesystem::file_size(sources.package, error);
  if (error) {
    journal.lastErrorMessage = "Can't read the game package.";
    return false;
  }
  journal.progressCounter = 0;
  journal.progressTotal = packageSize * 2;

  std::vector<uint8_t> data(packageSize);
  {
    std::ifstream file(sources.package, std::ios::binary);
    Hasher sha256(BCRYPT_SHA256_ALGORITHM);
    if (!file || !sha256.Valid()) {
      journal.lastErrorMessage = "Can't read the game package.";
      return false;
    }
    constexpr size_t kChunk = 4 << 20;
    for (uint64_t read = 0; read < packageSize;) {
      size_t count = size_t(std::min<uint64_t>(kChunk, packageSize - read));
      if (!file.read(reinterpret_cast<char*>(data.data() + read), std::streamsize(count))) {
        journal.lastErrorMessage = "Can't read the game package.";
        return false;
      }
      sha256.Update(data.data() + read, count);
      read += count;
      journal.progressCounter = read;
      if (!progressCallback()) {
        journal.lastErrorMessage = "Installation cancelled.";
        return false;
      }
    }
    std::vector<uint8_t> hash = sha256.Finish();
    if (!std::equal(hash.begin(), hash.end(), kPackageSha256.begin())) {
      journal.lastErrorMessage =
          "The game package doesn't match the supported World release (checksum mismatch).";
      return false;
    }
  }

  Stfs stfs(std::move(data));
  std::vector<Stfs::Entry> entries;
  std::string stfsError;
  if (!stfs.ReadEntries(entries, stfsError)) {
    journal.lastErrorMessage = "Invalid game package: " + stfsError + ".";
    return false;
  }
  if (!std::filesystem::exists(installPath, error)) {
    std::filesystem::create_directories(installPath, error);
    if (error) {
      journal.lastErrorMessage = "Can't create the installation folder.";
      return false;
    }
    journal.createdDirectories.push_front(installPath);
  }
  std::vector<uint8_t> contents;
  for (const Stfs::Entry& entry : entries) {
    std::filesystem::path target = installPath / entry.path;
    if (entry.directory) {
      if (!std::filesystem::exists(target, error)) {
        std::filesystem::create_directories(target, error);
        journal.createdDirectories.push_front(target);
      }
      continue;
    }
    if (!stfs.ReadFile(entry, contents, stfsError)) {
      journal.lastErrorMessage = "Invalid game package: " + stfsError + ".";
      return false;
    }
    std::filesystem::create_directories(target.parent_path(), error);
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    journal.createdFiles.push_back(target);
    if (!out.write(reinterpret_cast<const char*>(contents.data()),
                   std::streamsize(contents.size()))) {
      journal.lastErrorMessage = "Can't write " + entry.path.string() + ".";
      return false;
    }
    journal.progressCounter = std::min(journal.progressTotal,
                                       journal.progressCounter + contents.size());
    if (!progressCallback()) {
      journal.lastErrorMessage = "Installation cancelled.";
      return false;
    }
  }
  if (!checkInstall(installPath)) {
    journal.lastErrorMessage = "The game package doesn't contain the game.";
    return false;
  }
  journal.progressCounter = journal.progressTotal;
  progressCallback();
  return true;
}

void Installer::rollback(Journal& journal) {
  std::error_code error;
  for (const auto& file : journal.createdFiles) {
    std::filesystem::remove(file, error);
  }
  for (const auto& directory : journal.createdDirectories) {
    std::filesystem::remove_all(directory, error);
  }
  journal.createdFiles.clear();
  journal.createdDirectories.clear();
}
