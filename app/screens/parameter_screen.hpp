#pragma once

#include "screen.hpp"
#include "components/scrollable_list.hpp"
#include "components/search_bar.hpp"
#include "ros2_tui_launcher/node_inspector.hpp"
#include "ros2_tui_launcher/parameter_manager.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace rtl::tui {

/// Screen for browsing, editing, and dumping ROS 2 node parameters.
/// Two-panel layout: node list (left) + parameter table with detail view (right).
class ParameterScreen : public Screen {
public:
    ParameterScreen(NodeInspector* inspector, ParameterManager* param_mgr);

    std::string name() const override { return "Params"; }
    std::string hotkey() const override { return "P"; }
    ftxui::Component component() override;
    void tick() override;
    bool inputActive() const override;

private:
    /// Which panel has focus
    enum class Panel { Nodes, Params };

    /// Build the left panel (node list)
    ftxui::Element renderNodeList();

    /// Build the right panel (parameter table + detail)
    ftxui::Element renderParamPanel();

    /// Build the detail section for the selected parameter
    ftxui::Element renderParamDetail();

    /// Build the status/help bar at the bottom
    ftxui::Element renderStatusBar();

    /// Get the list of parameter names matching the current search filter
    std::vector<int> filteredParamIndices() const;

    NodeInspector* inspector_;
    ParameterManager* param_mgr_;

    std::mutex mutex_;
    std::vector<std::string> cached_node_names_;
    NodeParameters cached_params_;

    ScrollableList node_list_{ScrollableList::Config{true}};
    ScrollableList param_list_{ScrollableList::Config{true}};
    SearchBar search_bar_;

    Panel active_panel_ = Panel::Nodes;

    // Filtered parameter indices (updated each frame)
    std::vector<int> filtered_indices_;

    // Edit mode state
    bool editing_ = false;
    std::string edit_buffer_;
    std::string edit_param_name_;
    uint8_t edit_param_type_ = 0;
    std::string edit_error_;

    // Status message (e.g. "Dumped to file.yaml")
    std::string status_message_;
    std::chrono::steady_clock::time_point status_time_;
};

}  // namespace rtl::tui
