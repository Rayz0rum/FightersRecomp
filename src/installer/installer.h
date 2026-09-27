// Installing Sonic the Fighters' game data from the Xbox 360 World release
// package (STFS, title 5841129E, content type 000D0000) - verified against its
// SHA-256 and the package's own block hashes, then extracted. The interface
// (Journal, parse/install/rollback) follows UnleashedRecomp's install/installer
// (hedge-dev, GPL-3.0), which the wizard uses.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <string>

struct Journal {
  uint64_t progressCounter = 0;
  uint64_t progressTotal = 1;
  std::string lastErrorMessage;
  // Files and directories created, for the rollback.
  std::list<std::filesystem::path> createdFiles;
  std::list<std::filesystem::path> createdDirectories;
};

struct Installer {
  struct Sources {
    std::filesystem::path package;
    uint64_t totalSize = 0;
  };

  // The package, if the path is the package or a folder containing it
  // (such as the Xbox 360 content folder 5841129E).
  static bool parseGame(const std::filesystem::path& path, std::filesystem::path& package);
  // Whether the package is the supported one (checked before installing).
  static bool parseSources(const std::filesystem::path& package, Journal& journal,
                           Sources& sources);
  static bool checkInstall(const std::filesystem::path& installPath);
  // Verifies and extracts; progressCallback returning false cancels.
  static bool install(const Sources& sources, const std::filesystem::path& installPath,
                      Journal& journal, std::function<bool()> progressCallback);
  static void rollback(Journal& journal);
};
