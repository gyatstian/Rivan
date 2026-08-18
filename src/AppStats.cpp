// AppStats.cpp
// Listen-statistics preference wiring.
#include "App.h"

namespace rivan {

void App::SetStatsEnabled(bool enabled) {
    if (!ApplySettingsChange([enabled](config::AppSettings& settings) {
            if (settings.statsEnabled == enabled) return false;
            settings.statsEnabled = enabled;
            return true;
        })) return;
    stats_.SetEnabled(enabled);
    if (!enabled && settingsCategory_ == ui::SettingCategory::Statistics) {
        settingsCategory_ = ui::SettingCategory::Integrations;
    }
}

void App::SetStatisticsPeriod(const stats::DashboardPeriod period) {
    if (!settings_.Settings().statsEnabled || statisticsPeriod_ == period) return;
    statisticsPeriod_ = period;
    statisticsTracksPage_ = 0;
    statisticsArtistsPage_ = 0;
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetStatisticsTracksExpanded(const bool expanded) {
    if (!settings_.Settings().statsEnabled || statisticsTracksExpanded_ == expanded) return;
    statisticsTracksExpanded_ = expanded;
    statisticsTracksPage_ = 0;
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetStatisticsArtistsExpanded(const bool expanded) {
    if (!settings_.Settings().statsEnabled || statisticsArtistsExpanded_ == expanded) return;
    statisticsArtistsExpanded_ = expanded;
    statisticsArtistsPage_ = 0;
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetStatisticsTracksPage(const std::size_t page) {
    if (!settings_.Settings().statsEnabled || statisticsTracksExpanded_ ||
        statisticsTracksPage_ == page) return;
    statisticsTracksPage_ = page;
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetStatisticsArtistsPage(const std::size_t page) {
    if (!settings_.Settings().statsEnabled || statisticsArtistsExpanded_ ||
        statisticsArtistsPage_ == page) return;
    statisticsArtistsPage_ = page;
    ++revision_;
    if (window_) window_->Refresh();
}

} // namespace rivan
