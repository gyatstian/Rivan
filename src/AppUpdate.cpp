// AppUpdate.cpp
// Maps the asynchronous GitHub release service into the UI notifier model.
#include "App.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <shellapi.h>

namespace rivan {

void App::OnUpdateServiceUpdated() {
    const auto snapshot = update_.Snapshot();
    if (!snapshot || !snapshot->updateAvailable) return;
    updateSnapshot_ = snapshot;
    SetUpdateNotifierVisibleInternal(true);
}

void App::SetUpdateNotifierVisible(const bool visible) {
    SetUpdateNotifierVisibleInternal(visible);
}

void App::SetUpdateNotifierVisibleInternal(const bool visible) {
    if (visible && (!updateSnapshot_ || !updateSnapshot_->updateAvailable)) return;
    if (updateNotifierVisible_ == visible) return;
    updateNotifierVisible_ = visible;
    ++revision_;
    if (window_) window_->Refresh();
}

void App::OpenUpdateRelease() {
    // UpdateService accepts only the fixed GitHub releases path before it can publish a
    // snapshot. ShellExecute therefore receives an already validated HTTPS URL.
    if (!updateSnapshot_ || !updateSnapshot_->updateAvailable ||
        updateSnapshot_->releaseUrl.empty()) {
        return;
    }
    (void)ShellExecuteW(nullptr, L"open", updateSnapshot_->releaseUrl.c_str(), nullptr,
                        nullptr, SW_SHOWNORMAL);
}

} // namespace rivan
