#pragma once

#include "screen.hpp"
#include "components/scrollable_list.hpp"
#include "ros2_tui_launcher/launch_profile.hpp"
#include "ros2_tui_launcher/process_manager.hpp"
#include "ros2_tui_launcher/system_monitor.hpp"

#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rtl::tui {

/// Screen for managing launch profiles and their processes,
/// with system gauges and process tree view.
class LaunchScreen : public Screen {
public:
    LaunchScreen(
        std::vector<LaunchProfile>* profiles,
        int* active_profile_idx,
        ProcessManager* proc_mgr,
        SystemMonitor* sys_mon);
    ~LaunchScreen();

    std::string name() const override { return "Launch"; }
    std::string hotkey() const override { return "L"; }
    ftxui::Component component() override;
    void tick() override;

    /// Set callback invoked when the active profile changes.
    /// @param cb  Called with the new profile index.
    void setProfileChangeCallback(std::function<void(int)> cb) {
        on_profile_change_ = std::move(cb);
    }

private:
    void startSelected();
    void stopSelected();
    void restartSelected();
    void startAll();
    void stopAll();
    void nextProfile();

    /// Render the system gauge panel at the top.
    ftxui::Element renderGauges();

    /// Format memory in human-readable form (KB -> GB/MB).
    static std::string formatMemKb(unsigned long kb);
    static std::string formatMemMb(unsigned long mb);
    static std::string fmtPercent(double pct);

    std::vector<LaunchProfile>* profiles_;
    int* active_profile_idx_;
    ProcessManager* proc_mgr_;
    SystemMonitor* sys_mon_;

    std::mutex mutex_;
    std::unordered_map<std::string, ProcessInfo> cached_procs_;
    SystemInfo cached_sys_;
    std::unordered_map<std::string, ProcessTreeNode> cached_trees_;

    /// Track which entries are expanded (by display name).
    std::unordered_map<std::string, bool> expanded_;

    ScrollableList scroll_list_;

    /// Track background async operations so they are joined before destruction.
    std::mutex bg_mutex_;
    std::vector<std::future<void>> bg_futures_;
    void launchAsync(std::function<void()> fn);
    void cleanupFinishedFutures();

    std::function<void(int)> on_profile_change_;
};

}  // namespace rtl::tui
