#include "ros2_tui_launcher/node_inspector.hpp"

#include <spdlog/spdlog.h>

namespace rtl {

NodeInspector::NodeInspector(rclcpp::Node::SharedPtr node)
    : node_(node) {}

void NodeInspector::refresh() {
    // Throttle: skip if called too frequently
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(mutex_);
        if (now - last_refresh_ < kRefreshInterval) return;
        last_refresh_ = now;
    }

    doRefresh();
}

void NodeInspector::doRefresh() {
    // Perform all graph queries outside the mutex
    auto node_names = node_->get_node_names();
    auto all_services = node_->get_service_names_and_types();

    // Build the per-node info outside the lock
    std::unordered_map<std::string, DiscoveredNode> new_map;
    std::set<std::string> current_names(node_names.begin(), node_names.end());

    for (const auto& full_name : node_names) {
        DiscoveredNode dn;
        dn.full_name = full_name;

        // Parse namespace and name
        dn.ns = "/";
        dn.name = full_name;
        if (auto pos = full_name.rfind('/'); pos != std::string::npos && pos > 0) {
            dn.ns = full_name.substr(0, pos);
            dn.name = full_name.substr(pos + 1);
        } else if (full_name[0] == '/') {
            dn.name = full_name.substr(1);
        }

        // Check if this is a lifecycle node by looking for lifecycle services
        std::string lifecycle_prefix = full_name + "/get_state";
        for (const auto& [svc_name, _] : all_services) {
            if (svc_name.find(lifecycle_prefix) != std::string::npos ||
                svc_name.find(dn.name + "/get_state") != std::string::npos) {
                dn.is_lifecycle = true;
                break;
            }
        }

        new_map[full_name] = std::move(dn);
    }

    // Populate publishers and subscribers per node using graph introspection
    try {
        auto topic_names_and_types = node_->get_topic_names_and_types();
        for (const auto& [topic_name, types] : topic_names_and_types) {
            // Query publishers for this topic
            auto pub_endpoints = node_->get_publishers_info_by_topic(topic_name);
            for (const auto& ep : pub_endpoints) {
                std::string ep_ns = ep.node_namespace();
                std::string ep_name = ep.node_name();
                std::string node_full = (ep_ns == "/" ? "/" : ep_ns + "/") + ep_name;
                auto it = new_map.find(node_full);
                if (it != new_map.end()) {
                    it->second.publishers.push_back(topic_name);
                }
            }

            // Query subscribers for this topic
            auto sub_endpoints = node_->get_subscriptions_info_by_topic(topic_name);
            for (const auto& ep : sub_endpoints) {
                std::string ep_ns = ep.node_namespace();
                std::string ep_name = ep.node_name();
                std::string node_full = (ep_ns == "/" ? "/" : ep_ns + "/") + ep_name;
                auto it = new_map.find(node_full);
                if (it != new_map.end()) {
                    it->second.subscribers.push_back(topic_name);
                }
            }
        }
    } catch (const std::exception& e) {
        spdlog::debug("Failed to query topic endpoints: {}", e.what());
    }

    // Brief lock to swap in the new data
    std::lock_guard lock(mutex_);
    nodes_map_ = std::move(new_map);
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
    std::string service_name = node_ns + "/" + node_name + "/get_state";

    // Reuse cached client or create a new one
    rclcpp::Client<lifecycle_msgs::srv::GetState>::SharedPtr client;
    {
        std::lock_guard lock(mutex_);
        auto it = lifecycle_clients_.find(service_name);
        if (it != lifecycle_clients_.end()) {
            client = it->second;
        }
    }

    if (!client) {
        client = node_->create_client<lifecycle_msgs::srv::GetState>(service_name);
        std::lock_guard lock(mutex_);
        lifecycle_clients_[service_name] = client;
    }

    if (!client->wait_for_service(std::chrono::seconds(1))) {
        return "";
    }

    auto request = std::make_shared<lifecycle_msgs::srv::GetState::Request>();
    auto future = client->async_send_request(request);

    // Use spin_some with a deadline instead of spin_until_future_complete
    // to avoid conflicting with the main executor spinning the node
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (future.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready) {
            try {
                return future.get()->current_state.label;
            } catch (...) {
                return "";
            }
        }
    }

    return "";
}

}  // namespace rtl
