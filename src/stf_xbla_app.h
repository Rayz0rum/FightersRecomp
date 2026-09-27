#pragma once

#include <functional>
#include <optional>

#include <rex/rex_app.h>

class StfXblaApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) 
  {
    return std::unique_ptr<StfXblaApp>(
      new StfXblaApp(ctx, "stf_xbla", PPCImageConfig)
    );
  }

 protected:
  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnPreSetup(rex::RuntimeConfig& config) override;
  void OnPostSetup() override;
  void OnShutdown() override;
  void OnPreLaunchModule() override;
  // The installer (src/installer), if the game data isn't installed.
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) override;
  bool OnWindowCloseRequested() override;

};
