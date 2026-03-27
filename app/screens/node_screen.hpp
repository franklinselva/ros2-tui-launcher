#pragma once

#include "screen.hpp"
#include "ros2_tui_launcher/node_inspector.hpp"

#include <mutex>
#include <vector>

namespace rtl::tui {

/// Screen for inspecting active ROS 2 nodes.
class NodeScreen : public Screen {
public:
    explicit NodeScreen(NodeInspector* inspector);

    std::string name() const override { return "Nodes"; }
    std::string hotkey() const override { return "N"; }
    ftxui::Component component() override;
    void tick() override;

private:
    NodeInspector* inspector_;

    std::mutex mutex_;
    std::vector<DiscoveredNode> cached_nodes_;
    int selected_ = 0;
};

}  // namespace rtl::tui
