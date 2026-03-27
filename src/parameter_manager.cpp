#include "ros2_tui_launcher/parameter_manager.hpp"

#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>

#include <fstream>
#include <sstream>

namespace rtl {

ParameterManager::ParameterManager(rclcpp::Node::SharedPtr node)
    : node_(std::move(node)) {}

void ParameterManager::requestNodeParams(const std::string& node_full_name) {
    doFetch(node_full_name, false);
}

void ParameterManager::forceRefresh(const std::string& node_full_name) {
    doFetch(node_full_name, true);
}

NodeParameters ParameterManager::getNodeParams(const std::string& node_full_name) const {
    std::lock_guard lock(mutex_);
    auto it = nodes_.find(node_full_name);
    if (it == nodes_.end()) return {};
    return it->second.params;
}

void ParameterManager::doFetch(const std::string& node_name, bool force) {
    std::lock_guard lock(mutex_);

    auto& state = nodes_[node_name];
    state.params.node_name = node_name;

    // Throttle
    auto now = std::chrono::steady_clock::now();
    if (!force && state.params.loading) return;
    if (!force && now - state.last_fetch < kFetchInterval) return;

    // Create client if needed
    if (!state.client) {
        try {
            state.client = std::make_shared<rclcpp::AsyncParametersClient>(
                node_, node_name);
        } catch (const std::exception& e) {
            state.params.error = std::string("Failed to create client: ") + e.what();
            spdlog::debug("ParameterManager: {}", state.params.error);
            return;
        }
    }

    if (!state.client->service_is_ready()) {
        state.params.error = "Parameter service not available";
        state.params.loaded = false;
        return;
    }

    state.params.loading = true;
    state.params.error.clear();
    state.last_fetch = now;

    // Step 1: List all parameters
    auto client = state.client;
    auto node_name_copy = node_name;

    client->list_parameters({}, 0,
        [this, client, node_name_copy](
            std::shared_future<rcl_interfaces::msg::ListParametersResult> future)
        {
            rcl_interfaces::msg::ListParametersResult result;
            try {
                result = future.get();
            } catch (const std::exception& e) {
                std::lock_guard lock(mutex_);
                auto it = nodes_.find(node_name_copy);
                if (it != nodes_.end()) {
                    it->second.params.loading = false;
                    it->second.params.error = std::string("List failed: ") + e.what();
                }
                return;
            }

            if (result.names.empty()) {
                std::lock_guard lock(mutex_);
                auto it = nodes_.find(node_name_copy);
                if (it != nodes_.end()) {
                    it->second.params.params.clear();
                    it->second.params.loaded = true;
                    it->second.params.loading = false;
                }
                return;
            }

            auto names = result.names;

            // Step 2: Get values and descriptions in parallel
            client->get_parameters(names,
                [this, client, node_name_copy, names](
                    std::shared_future<std::vector<rclcpp::Parameter>> val_future)
                {
                    std::vector<rclcpp::Parameter> values;
                    try {
                        values = val_future.get();
                    } catch (const std::exception& e) {
                        std::lock_guard lock(mutex_);
                        auto it = nodes_.find(node_name_copy);
                        if (it != nodes_.end()) {
                            it->second.params.loading = false;
                            it->second.params.error = std::string("Get failed: ") + e.what();
                        }
                        return;
                    }

                    // Step 3: Describe parameters for metadata
                    client->describe_parameters(names,
                        [this, node_name_copy, names, values](
                            std::shared_future<std::vector<rcl_interfaces::msg::ParameterDescriptor>> desc_future)
                        {
                            std::vector<rcl_interfaces::msg::ParameterDescriptor> descriptors;
                            try {
                                descriptors = desc_future.get();
                            } catch (...) {
                                // Descriptions are optional — proceed without them
                            }

                            // Assemble ParameterInfo structs
                            std::vector<ParameterInfo> params;
                            params.reserve(names.size());

                            for (size_t i = 0; i < names.size(); ++i) {
                                ParameterInfo pi;
                                pi.name = names[i];

                                if (i < values.size()) {
                                    pi.type = static_cast<uint8_t>(values[i].get_type());
                                    pi.type_name = paramTypeName(pi.type);
                                    pi.value_str = paramValueToString(values[i]);
                                }

                                if (i < descriptors.size()) {
                                    pi.description = descriptors[i].description;
                                    pi.read_only = descriptors[i].read_only;

                                    if (!descriptors[i].integer_range.empty()) {
                                        pi.has_integer_range = true;
                                        pi.int_range_min = descriptors[i].integer_range[0].from_value;
                                        pi.int_range_max = descriptors[i].integer_range[0].to_value;
                                        pi.int_range_step = descriptors[i].integer_range[0].step;
                                    }
                                    if (!descriptors[i].floating_point_range.empty()) {
                                        pi.has_float_range = true;
                                        pi.float_range_min = descriptors[i].floating_point_range[0].from_value;
                                        pi.float_range_max = descriptors[i].floating_point_range[0].to_value;
                                        pi.float_range_step = descriptors[i].floating_point_range[0].step;
                                    }
                                }

                                params.push_back(std::move(pi));
                            }

                            // Sort by name for stable display
                            std::sort(params.begin(), params.end(),
                                [](const ParameterInfo& a, const ParameterInfo& b) {
                                    return a.name < b.name;
                                });

                            std::lock_guard lock(mutex_);
                            auto it = nodes_.find(node_name_copy);
                            if (it != nodes_.end()) {
                                it->second.params.params = std::move(params);
                                it->second.params.loaded = true;
                                it->second.params.loading = false;
                                it->second.params.error.clear();
                            }
                        });
                });
        });
}

void ParameterManager::setParameter(const std::string& node_full_name,
                                     const std::string& param_name,
                                     const std::string& value_str,
                                     uint8_t param_type,
                                     SetParamCallback cb) {
    std::shared_ptr<rclcpp::AsyncParametersClient> client;
    {
        std::lock_guard lock(mutex_);
        auto it = nodes_.find(node_full_name);
        if (it == nodes_.end() || !it->second.client) {
            if (cb) cb(false, "No client for node");
            return;
        }
        client = it->second.client;
    }

    rclcpp::Parameter param;
    try {
        param = parseValue(param_name, value_str, param_type);
    } catch (const std::exception& e) {
        if (cb) cb(false, std::string("Parse error: ") + e.what());
        return;
    }

    auto node_name_copy = node_full_name;
    client->set_parameters({param},
        [this, cb, node_name_copy](
            std::shared_future<std::vector<rcl_interfaces::msg::SetParametersResult>> future)
        {
            try {
                auto results = future.get();
                if (!results.empty() && results[0].successful) {
                    if (cb) cb(true, "");
                    // Re-fetch to update cache
                    forceRefresh(node_name_copy);
                } else {
                    std::string reason = results.empty() ? "No result" : results[0].reason;
                    if (cb) cb(false, reason);
                }
            } catch (const std::exception& e) {
                if (cb) cb(false, std::string("Set failed: ") + e.what());
            }
        });
}

bool ParameterManager::dumpToYaml(const std::string& node_full_name,
                                   const std::filesystem::path& output_path) const {
    NodeParameters np;
    {
        std::lock_guard lock(mutex_);
        auto it = nodes_.find(node_full_name);
        if (it == nodes_.end() || !it->second.params.loaded) return false;
        np = it->second.params;
    }

    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << node_full_name;
    out << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "ros__parameters";
    out << YAML::Value << YAML::BeginMap;

    for (const auto& p : np.params) {
        out << YAML::Key << p.name;
        // Emit typed values
        using PT = rcl_interfaces::msg::ParameterType;
        switch (p.type) {
            case PT::PARAMETER_BOOL:
                out << YAML::Value << (p.value_str == "true");
                break;
            case PT::PARAMETER_INTEGER:
                try { out << YAML::Value << std::stoll(p.value_str); }
                catch (...) { out << YAML::Value << p.value_str; }
                break;
            case PT::PARAMETER_DOUBLE:
                try { out << YAML::Value << std::stod(p.value_str); }
                catch (...) { out << YAML::Value << p.value_str; }
                break;
            default:
                out << YAML::Value << p.value_str;
                break;
        }
    }

    out << YAML::EndMap;  // ros__parameters
    out << YAML::EndMap;  // node_full_name
    out << YAML::EndMap;  // root

    std::ofstream file(output_path);
    if (!file.is_open()) {
        spdlog::error("Failed to open {} for writing", output_path.string());
        return false;
    }
    file << out.c_str() << "\n";
    spdlog::info("Dumped parameters for '{}' to {}", node_full_name, output_path.string());
    return true;
}

void ParameterManager::pruneNodes(const std::vector<std::string>& active_node_names) {
    std::lock_guard lock(mutex_);
    std::unordered_set<std::string> active_set(active_node_names.begin(), active_node_names.end());
    for (auto it = nodes_.begin(); it != nodes_.end(); ) {
        if (active_set.find(it->first) == active_set.end()) {
            it = nodes_.erase(it);
        } else {
            ++it;
        }
    }
}

std::string ParameterManager::paramValueToString(const rclcpp::Parameter& param) {
    using PT = rclcpp::ParameterType;
    switch (param.get_type()) {
        case PT::PARAMETER_BOOL:
            return param.as_bool() ? "true" : "false";
        case PT::PARAMETER_INTEGER:
            return std::to_string(param.as_int());
        case PT::PARAMETER_DOUBLE: {
            std::ostringstream oss;
            oss << param.as_double();
            return oss.str();
        }
        case PT::PARAMETER_STRING:
            return param.as_string();
        case PT::PARAMETER_BYTE_ARRAY: {
            auto v = param.as_byte_array();
            std::ostringstream oss;
            oss << "[";
            for (size_t i = 0; i < v.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << static_cast<int>(v[i]);
            }
            oss << "]";
            return oss.str();
        }
        case PT::PARAMETER_BOOL_ARRAY: {
            auto v = param.as_bool_array();
            std::ostringstream oss;
            oss << "[";
            for (size_t i = 0; i < v.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << (v[i] ? "true" : "false");
            }
            oss << "]";
            return oss.str();
        }
        case PT::PARAMETER_INTEGER_ARRAY: {
            auto v = param.as_integer_array();
            std::ostringstream oss;
            oss << "[";
            for (size_t i = 0; i < v.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << v[i];
            }
            oss << "]";
            return oss.str();
        }
        case PT::PARAMETER_DOUBLE_ARRAY: {
            auto v = param.as_double_array();
            std::ostringstream oss;
            oss << "[";
            for (size_t i = 0; i < v.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << v[i];
            }
            oss << "]";
            return oss.str();
        }
        case PT::PARAMETER_STRING_ARRAY: {
            auto v = param.as_string_array();
            std::ostringstream oss;
            oss << "[";
            for (size_t i = 0; i < v.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << "\"" << v[i] << "\"";
            }
            oss << "]";
            return oss.str();
        }
        default:
            return "<not set>";
    }
}

std::string ParameterManager::paramTypeName(uint8_t type) {
    using PT = rcl_interfaces::msg::ParameterType;
    switch (type) {
        case PT::PARAMETER_BOOL:           return "bool";
        case PT::PARAMETER_INTEGER:        return "int";
        case PT::PARAMETER_DOUBLE:         return "double";
        case PT::PARAMETER_STRING:         return "string";
        case PT::PARAMETER_BYTE_ARRAY:     return "byte[]";
        case PT::PARAMETER_BOOL_ARRAY:     return "bool[]";
        case PT::PARAMETER_INTEGER_ARRAY:  return "int[]";
        case PT::PARAMETER_DOUBLE_ARRAY:   return "double[]";
        case PT::PARAMETER_STRING_ARRAY:   return "string[]";
        default:                           return "unknown";
    }
}

rclcpp::Parameter ParameterManager::parseValue(const std::string& name,
                                                 const std::string& value_str,
                                                 uint8_t type) {
    using PT = rcl_interfaces::msg::ParameterType;
    switch (type) {
        case PT::PARAMETER_BOOL:
            if (value_str == "true" || value_str == "1")
                return rclcpp::Parameter(name, true);
            if (value_str == "false" || value_str == "0")
                return rclcpp::Parameter(name, false);
            throw std::invalid_argument("Expected 'true' or 'false'");
        case PT::PARAMETER_INTEGER:
            return rclcpp::Parameter(name, static_cast<int64_t>(std::stoll(value_str)));
        case PT::PARAMETER_DOUBLE:
            return rclcpp::Parameter(name, std::stod(value_str));
        case PT::PARAMETER_STRING:
            return rclcpp::Parameter(name, value_str);
        default:
            throw std::invalid_argument("Editing this parameter type is not supported");
    }
}

}  // namespace rtl
