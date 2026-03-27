#pragma once

#include "screen.hpp"
#include "ros2_tui_launcher/topic_monitor.hpp"

#include <mutex>
#include <vector>

namespace rtl::tui {

/// Screen for monitoring topic frequencies and health.
class TopicScreen : public Screen {
public:
    explicit TopicScreen(TopicMonitor* topic_mon);

    std::string name() const override { return "Topics"; }
    std::string hotkey() const override { return "T"; }
    ftxui::Component component() override;
    void tick() override;

private:
    TopicMonitor* topic_mon_;

    std::mutex mutex_;
    std::vector<TopicInfo> cached_topics_;
    bool show_all_ = false;  // false = only watched topics, true = all
};

}  // namespace rtl::tui
