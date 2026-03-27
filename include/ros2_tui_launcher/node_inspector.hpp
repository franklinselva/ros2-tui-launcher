#pragma once

#include <rclcpp/rclcpp.hpp>
#include <lifecycle_msgs/srv/get_state.hpp>

#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>

namespace rtl {

/// Information about a discovered ROS 2 node.
struct DiscoveredNode {
    std::string name;
    std::string ns;                          ///< Namespace
    std::string full_name;                   ///< /ns/name
    std::vector<std::string> publishers;     ///< Topic names this node publishes
    std::vector<std::string> subscribers;    ///< Topic names this node subscribes to
    std::vector<std::string> services;       ///< Services offered
    std::string lifecycle_state;             ///< Empty if not a lifecycle node
    bool is_lifecycle = false;
};

/// Uses rclcpp graph introspection to discover and inspect nodes.
/// Must be used from a thread that can call rclcpp spin functions.
class NodeInspector {
public:
    explicit NodeInspector(rclcpp::Node::SharedPtr node);

    /// Refresh the list of discovered nodes from the ROS graph.
    void refresh();

    /// Get the current snapshot of discovered nodes.
    std::vector<DiscoveredNode> nodes() const;

    /// Get info for a specific node.
    DiscoveredNode nodeInfo(const std::string& full_name) const;

    /// Query lifecycle state for a node. Returns empty string if not a lifecycle node.
    std::string queryLifecycleState(const std::string& node_name, const std::string& node_ns);

private:
    rclcpp::Node::SharedPtr node_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, DiscoveredNode> nodes_map_;
};

}  // namespace rtl
