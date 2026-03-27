#pragma once

#include "screen.hpp"
#include "components/scrollable_list.hpp"
#include "ros2_tui_launcher/launch_profile.hpp"
#include "ros2_tui_launcher/process_manager.hpp"

#include <mutex>
#include <vector>

namespace rtl::tui {

/// Screen for managing launch profiles and their processes.
class LaunchScreen : public Screen {
public:
    LaunchScreen(
        std::vector<LaunchProfile>* profiles,
        int* active_profile_idx,
        ProcessManager* proc_mgr);

    std::string name() const override { return "Launch"; }
    std::string hotkey() const override { return "L"; }
    ftxui::Component component() override;
    void tick() override;

private:
    void startSelected();
    void stopSelected();
    void restartSelected();
    void startAll();
    void stopAll();
    void nextProfile();

    std::vector<LaunchProfile>* profiles_;
    int* active_profile_idx_;
    ProcessManager* proc_mgr_;

    std::mutex mutex_;
    std::vector<ProcessInfo> cached_procs_;

    ScrollableList scroll_list_;
};

}  // namespace rtl::tui
