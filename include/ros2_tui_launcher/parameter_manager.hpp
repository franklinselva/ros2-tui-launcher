#pragma once

#include <rclcpp/rclcpp.hpp>
#include <rcl_interfaces/msg/parameter_descriptor.hpp>
#include <rcl_interfaces/msg/parameter_type.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rtl {

/// Information about a single parameter on a remote node.
struct ParameterInfo {
    std::string name;
    uint8_t type = 0;           ///< rcl_interfaces::msg::ParameterType constant
    std::string type_name;      ///< Human-readable: "bool", "int", "double", "string", etc.
    std::string value_str;      ///< String representation of current value
    std::string description;
    bool read_only = false;
    bool has_integer_range = false;
    int64_t int_range_min = 0;
    int64_t int_range_max = 0;
    uint64_t int_range_step = 0;
    bool has_float_range = false;
    double float_range_min = 0.0;
    double float_range_max = 0.0;
    double float_range_step = 0.0;
};

/// Cached parameters for one node.
struct NodeParameters {
    std::string node_name;
    std::vector<ParameterInfo> params;
    bool loaded = false;        ///< True if params have been fetched at least once
    bool loading = false;       ///< True if a fetch is in progress
    std::string error;          ///< Non-empty if the last fetch failed
};

/// Callback for set-parameter results.
using SetParamCallback = std::function<void(bool success, const std::string& message)>;

/// Manages parameter queries for remote ROS 2 nodes.
/// Uses AsyncParametersClient for non-blocking operations.
/// Thread-safe: all public methods can be called from any thread.
class ParameterManager {
public:
    explicit ParameterManager(rclcpp::Node::SharedPtr node);

    /// Request fetching parameters for a node.
    /// Non-blocking. Results are available via getNodeParams() after completion.
    /// Throttled: repeated calls within kFetchInterval are no-ops.
    void requestNodeParams(const std::string& node_full_name);

    /// Force a re-fetch of parameters for a node (ignores throttle).
    void forceRefresh(const std::string& node_full_name);

    /// Get cached parameters for a node. Returns a copy (thread-safe).
    NodeParameters getNodeParams(const std::string& node_full_name) const;

    /// Set a parameter on a remote node. Non-blocking.
    /// The value_str is parsed according to the parameter's current type.
    void setParameter(const std::string& node_full_name,
                      const std::string& param_name,
                      const std::string& value_str,
                      uint8_t param_type,
                      SetParamCallback cb = {});

    /// Dump cached parameters for a node to a ROS 2 parameter YAML file.
    /// Returns true on success. Uses cached data (no network call).
    bool dumpToYaml(const std::string& node_full_name,
                    const std::filesystem::path& output_path) const;

    /// Remove cached state for nodes not in the provided list.
    void pruneNodes(const std::vector<std::string>& active_node_names);

private:
    struct NodeState {
        NodeParameters params;
        std::shared_ptr<rclcpp::AsyncParametersClient> client;
        std::chrono::steady_clock::time_point last_fetch{};
    };

    void doFetch(const std::string& node_name, bool force);

    /// Convert a ParameterValue to a display string.
    static std::string paramValueToString(const rclcpp::Parameter& param);

    /// Get human-readable type name from ParameterType constant.
    static std::string paramTypeName(uint8_t type);

    /// Parse a string value into an rclcpp::Parameter.
    static rclcpp::Parameter parseValue(const std::string& name,
                                        const std::string& value_str,
                                        uint8_t type);

    rclcpp::Node::SharedPtr node_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, NodeState> nodes_;

    static constexpr std::chrono::milliseconds kFetchInterval{3000};
};

}  // namespace rtl
