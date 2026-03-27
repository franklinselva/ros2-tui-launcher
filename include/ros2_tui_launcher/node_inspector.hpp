#pragma once

#include <rclcpp/rclcpp.hpp>
#include <lifecycle_msgs/srv/get_state.hpp>

#include <chrono>
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
class NodeInspector {
public:
    explicit NodeInspector(rclcpp::Node::SharedPtr node);

    /// Refresh the list of discovered nodes from the ROS graph.
    /// Throttled internally — safe to call at high frequency.
    void refresh();

    /// Get the current snapshot of discovered nodes.
    std::vector<DiscoveredNode> nodes() const;

    /// Get info for a specific node.
    DiscoveredNode nodeInfo(const std::string& full_name) const;

    /// Query lifecycle state for a node. Returns empty string if not a lifecycle node.
    std::string queryLifecycleState(const std::string& node_name, const std::string& node_ns);

private:
    void doRefresh();

    rclcpp::Node::SharedPtr node_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, DiscoveredNode> nodes_map_;

    // Throttle: minimum 2s between actual refreshes
    std::chrono::steady_clock::time_point last_refresh_{};
    static constexpr std::chrono::milliseconds kRefreshInterval{2000};

    // Cached lifecycle service clients to avoid creating them on every call
    std::unordered_map<std::string, rclcpp::Client<lifecycle_msgs::srv::GetState>::SharedPtr>
        lifecycle_clients_;
};

}  // namespace rtl
