// SettingsManager.cpp
// Coordinates durable preferences and resumable session persistence.
#include "SettingsManager.h"

#include "../core/AppPaths.h"

#include <utility>

namespace rivan::config {

SettingsManager::SettingsManager()
    : SettingsManager(core::AppPaths::SettingsFile(), core::AppPaths::SessionFile()) {}

SettingsManager::SettingsManager(std::filesystem::path settingsFile,
                                 std::filesystem::path sessionFile)
    : settingsFile_(std::move(settingsFile)),
      sessionFile_(std::move(sessionFile)),
      settings_(AppSettings::Defaults()),
      session_(SessionState::Defaults()) {}

bool SettingsManager::Load(std::string* error, std::string* warnings) {
    if (warnings != nullptr) warnings->clear();
    if (!LoadSettings(error, warnings)) return false;
    return LoadSession(error, warnings);
}

bool SettingsManager::Save(std::string* error) const {
    return SaveSettings(error) && SaveSession(error);
}

} // namespace rivan::config
