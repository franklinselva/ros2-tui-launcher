#include "ros2_tui_launcher/node_inspector.hpp"

#include <spdlog/spdlog.h>

namespace rtl {

NodeInspector::NodeInspector(rclcpp::Node::SharedPtr node)
    : node_(node) {}

void NodeInspector::refresh() {
    auto node_names = node_->get_node_names();

    std::lock_guard lock(mutex_);
    // Mark all existing as potentially stale
    std::set<std::string> current_names;

    for (const auto& full_name : node_names) {
        current_names.insert(full_name);

        // Parse namespace and name
        std::string ns = "/";
        std::string name = full_name;
        if (auto pos = full_name.rfind('/'); pos != std::string::npos && pos > 0) {
            ns = full_name.substr(0, pos);
            name = full_name.substr(pos + 1);
        } else if (full_name[0] == '/') {
            name = full_name.substr(1);
        }

        auto& dn = nodes_map_[full_name];
        dn.full_name = full_name;
        dn.name = name;
        dn.ns = ns;

        // Get topic info
        try {
            auto pubs = node_->get_publishers_info_by_topic(full_name);
            // Instead, query all topics and filter
        } catch (...) {}
    }

    // Get all topics and map them to nodes
    auto topic_map = node_->get_topic_names_and_types();
    for (auto& [_, dn] : nodes_map_) {
        dn.publishers.clear();
        dn.subscribers.clear();
        dn.services.clear();
    }

    // Use graph API to get per-node info
    for (const auto& full_name : node_names) {
        if (nodes_map_.find(full_name) == nodes_map_.end()) continue;
        auto& dn = nodes_map_[full_name];

        // Parse name/ns for API calls
        std::string node_name = dn.name;
        std::string node_ns = dn.ns;

        try {
            // Get publishers
            for (const auto& [topic, _] : topic_map) {
                auto pubs = node_->count_publishers(topic);
                auto subs = node_->count_subscribers(topic);
                // Note: rclcpp doesn't provide per-node topic mapping directly
                // without get_publishers_info_by_topic
                (void)pubs;
                (void)subs;
            }
        } catch (...) {}

        // Check if this is a lifecycle node by looking for lifecycle services
        try {
            auto services = node_->get_service_names_and_types();
            std::string lifecycle_prefix = full_name + "/get_state";
            for (const auto& [svc_name, _] : services) {
                if (svc_name.find(lifecycle_prefix) != std::string::npos ||
                    svc_name.find(node_name + "/get_state") != std::string::npos) {
                    dn.is_lifecycle = true;
                    break;
                }
            }
        } catch (...) {}
    }

    // Remove nodes that are no longer present
    for (auto it = nodes_map_.begin(); it != nodes_map_.end();) {
        if (current_names.find(it->first) == current_names.end()) {
            it = nodes_map_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<DiscoveredNode> NodeInspector::nodes() const {
    std::lock_guard lock(mutex_);
    std::vector<DiscoveredNode> result;
    result.reserve(nodes_map_.size());
    for (const auto& [_, dn] : nodes_map_) {
        result.push_back(dn);
    }
    return result;
}

DiscoveredNode NodeInspector::nodeInfo(const std::string& full_name) const {
    std::lock_guard lock(mutex_);
    auto it = nodes_map_.find(full_name);
    if (it == nodes_map_.end()) return {};
    return it->second;
}

std::string NodeInspector::queryLifecycleState(
    const std::string& node_name, const std::string& node_ns)
{
    auto client = node_->create_client<lifecycle_msgs::srv::GetState>(
        node_ns + "/" + node_name + "/get_state");

    if (!client->wait_for_service(std::chrono::seconds(1))) {
        return "";
    }

    auto request = std::make_shared<lifecycle_msgs::srv::GetState::Request>();
    auto future = client->async_send_request(request);

    if (rclcpp::spin_until_future_complete(node_, future, std::chrono::seconds(2))
        == rclcpp::FutureReturnCode::SUCCESS) {
        return future.get()->current_state.label;
    }

    return "";
}

}  // namespace rtl
