#include "ros2_tui_launcher/config_validator.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <unordered_set>

namespace rtl {

bool ConfigValidator::isValidRosParamName(const std::string& name) {
    if (name.empty()) return false;
    // First char: letter or underscore
    char c = name[0];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'))
        return false;
    // Remaining: alphanumeric, underscore, dot, or slash
    for (size_t i = 1; i < name.size(); ++i) {
        c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '/'))
            return false;
    }
    return true;
}

bool ConfigValidator::isValidEnvVarName(const std::string& name) {
    if (name.empty()) return false;
    char c = name[0];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'))
        return false;
    for (size_t i = 1; i < name.size(); ++i) {
        c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

void ConfigValidator::validateProfileFields(const LaunchProfile& profile,
                                             ValidationResult& result) const {
    if (profile.name.empty()) {
        result.errors.push_back({"", "name", "Profile name is empty", true});
        result.valid = false;
    }

    if (profile.topic_hz_rate <= 0) {
        result.errors.push_back({"", "topic_hz_rate",
            "topic_hz_rate must be > 0, got " + std::to_string(profile.topic_hz_rate),
            false});
    }
}

void ConfigValidator::validateEntry(const LaunchEntry& entry, size_t index,
                                     const std::vector<LaunchEntry>& all_entries,
                                     ValidationResult& result) const {
    std::string ename = entry.displayName();
    if (ename.empty() || ename == "/") {
        ename = "entry[" + std::to_string(index) + "]";
    }

    // package is required
    if (entry.package.empty()) {
        result.errors.push_back({ename, "package", "Package name is required", false});
        result.invalid_entry_indices.push_back(index);
        return;  // No point validating further
    }

    // Exactly one of launch_file or executable
    if (entry.launch_file.empty() && entry.executable.empty()) {
        result.errors.push_back({ename, "launch_file/executable",
            "Either 'launch_file' or 'executable' must be set", false});
        result.invalid_entry_indices.push_back(index);
        return;
    }
    if (!entry.launch_file.empty() && !entry.executable.empty()) {
        result.errors.push_back({ename, "launch_file/executable",
            "Only one of 'launch_file' or 'executable' should be set, not both", false});
    }

    // restart_policy must be a known value
    static const std::unordered_set<std::string> valid_policies = {
        "never", "on-failure", "always"
    };
    if (valid_policies.find(entry.restart_policy) == valid_policies.end()) {
        result.errors.push_back({ename, "restart_policy",
            "Invalid restart_policy '" + entry.restart_policy +
            "', must be 'never', 'on-failure', or 'always'", false});
    }

    // params_file existence check
    if (!entry.params_file.empty()) {
        if (!std::filesystem::exists(entry.params_file)) {
            result.errors.push_back({ename, "params_file",
                "File not found: " + entry.params_file, false});
        }
    }

    // Validate parameter keys
    for (const auto& [key, _] : entry.parameters) {
        if (!isValidRosParamName(key)) {
            result.errors.push_back({ename, "parameters",
                "Invalid parameter name '" + key +
                "' (must match [a-zA-Z_][a-zA-Z0-9_./]*)", false});
        }
    }

    // Validate env var keys
    for (const auto& [key, _] : entry.env) {
        if (!isValidEnvVarName(key)) {
            result.errors.push_back({ename, "env",
                "Invalid environment variable name '" + key +
                "' (must match [a-zA-Z_][a-zA-Z0-9_]*)", false});
        }
    }

    // Check for duplicate display names
    for (size_t j = 0; j < index; ++j) {
        if (all_entries[j].displayName() == entry.displayName()) {
            result.errors.push_back({ename, "name",
                "Duplicate entry name '" + ename + "'", false});
            break;
        }
    }
}

void ConfigValidator::validateMonitoredTopics(const LaunchProfile& profile,
                                               ValidationResult& result) const {
    for (size_t i = 0; i < profile.monitored_topics.size(); ++i) {
        const auto& mt = profile.monitored_topics[i];
        std::string label = "monitor.topics[" + std::to_string(i) + "]";

        if (!mt.topic.empty() && mt.topic[0] != '/') {
            result.errors.push_back({"", label,
                "Topic name '" + mt.topic + "' should start with '/'", false});
        }

        if (mt.expected_hz < 0) {
            result.errors.push_back({"", label,
                "expected_hz must be >= 0, got " + std::to_string(mt.expected_hz),
                false});
        }
    }
}

void ConfigValidator::validateLogConfig(const LogConfig& config,
                                         ValidationResult& result) const {
    if (config.max_file_size_bytes == 0) {
        result.errors.push_back({"", "logging.max_file_size_mb",
            "max_file_size must be > 0", false});
    }

    if (config.flush_interval.count() <= 0) {
        result.errors.push_back({"", "logging.flush_interval_ms",
            "flush_interval must be > 0", false});
    }
}

ValidationResult ConfigValidator::validate(const LaunchProfile& profile,
                                            const std::filesystem::path& source_file) const {
    ValidationResult result;

    validateProfileFields(profile, result);
    validateMonitoredTopics(profile, result);
    validateLogConfig(profile.log_config, result);

    for (size_t i = 0; i < profile.entries.size(); ++i) {
        validateEntry(profile.entries[i], i, profile.entries, result);
    }

    // Log all errors
    for (const auto& err : result.errors) {
        std::string location = source_file.string();
        if (!err.entry_name.empty()) {
            location += " [" + err.entry_name + "]";
        }
        if (err.critical) {
            spdlog::error("{}: {}: {}", location, err.field, err.message);
        } else {
            spdlog::warn("{}: {}: {}", location, err.field, err.message);
        }
    }

    return result;
}

}  // namespace rtl
