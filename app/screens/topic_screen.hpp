#pragma once

#include "screen.hpp"
#include "components/scrollable_list.hpp"
#include "components/search_bar.hpp"
#include "components/filter_dropdown.hpp"
#include "ros2_tui_launcher/topic_monitor.hpp"
#include "ros2_tui_launcher/node_inspector.hpp"

#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace rtl::tui {

/// Screen for monitoring topic frequencies and health.
class TopicScreen : public Screen {
public:
    TopicScreen(TopicMonitor* topic_mon, NodeInspector* node_inspector);

    std::string name() const override { return "Topics"; }
    std::string hotkey() const override { return "T"; }
    ftxui::Component component() override;
    void tick() override;
    bool inputActive() const override {
        return search_bar_.inputActive() || node_filter_.inputActive();
    }

private:
    TopicMonitor* topic_mon_;
    NodeInspector* node_inspector_;

    std::mutex mutex_;
    std::vector<TopicInfo> cached_topics_;
    bool show_all_ = false;

    // node full_name -> set of topic names (publishers + subscribers)
    std::unordered_map<std::string, std::set<std::string>> cached_node_topics_;

    ScrollableList scroll_list_;
    SearchBar search_bar_;
    FilterDropdown node_filter_{"f", "Node", ftxui::Color::Magenta};
};

}  // namespace rtl::tui
