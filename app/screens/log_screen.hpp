#pragma once

#include "screen.hpp"
#include "components/scrollable_list.hpp"
#include "components/search_bar.hpp"
#include "components/filter_dropdown.hpp"
#include "ros2_tui_launcher/log_aggregator.hpp"
#include "ros2_tui_launcher/node_inspector.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace rtl::tui {

/// Screen for viewing and filtering aggregated logs.
class LogScreen : public Screen {
public:
    LogScreen(LogAggregator* log_agg, NodeInspector* node_inspector);

    std::string name() const override { return "Logs"; }
    std::string hotkey() const override { return "G"; }
    ftxui::Component component() override;
    void tick() override;
    bool inputActive() const override {
        return search_bar_.inputActive()
            || source_filter_.inputActive()
            || level_filter_.inputActive()
            || node_filter_.inputActive();
    }

private:
    LogAggregator* log_agg_;
    NodeInspector* node_inspector_;

    std::mutex mutex_;
    std::vector<LogEntry> cached_entries_;
    uint64_t last_log_gen_ = 0;
    std::string last_source_filter_;
    std::string last_search_;
    int last_level_idx_ = 0;
    std::string last_node_filter_;

    ScrollableList scroll_list_{ScrollableList::Config{false, 3, true}};
    SearchBar search_bar_;
    FilterDropdown source_filter_{"f", "Source", ftxui::Color::Cyan};
    FilterDropdown level_filter_{"v", "Level", ftxui::Color::Yellow};
    FilterDropdown node_filter_{"n", "Node", ftxui::Color::Magenta};
};

}  // namespace rtl::tui
