#pragma once

#include "screen.hpp"
#include "ros2_tui_launcher/log_aggregator.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace rtl::tui {

/// Screen for viewing and filtering aggregated logs.
class LogScreen : public Screen {
public:
    explicit LogScreen(LogAggregator* log_agg);

    std::string name() const override { return "Logs"; }
    std::string hotkey() const override { return "G"; }
    ftxui::Component component() override;
    void tick() override;

private:
    LogAggregator* log_agg_;

    std::mutex mutex_;
    std::vector<LogEntry> cached_entries_;
    std::vector<std::string> cached_sources_;

    int selected_source_ = 0;       // 0 = all
    int selected_level_ = 0;        // 0 = Debug, 1 = Info, etc.
    std::string search_text_;
    int scroll_offset_ = 0;
    bool auto_scroll_ = true;
    bool search_mode_ = false;
    int viewport_height_ = 40;      // updated dynamically from terminal size
};

}  // namespace rtl::tui
