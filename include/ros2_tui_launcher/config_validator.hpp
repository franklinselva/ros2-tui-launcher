#pragma once

#include "ros2_tui_launcher/launch_profile.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace rtl {

/// A single validation error found in a profile.
struct ValidationError {
    std::string entry_name;   ///< Empty for profile-level errors
    std::string field;        ///< Field that failed validation
    std::string message;      ///< Human-readable description
    bool critical;            ///< If true, the entire profile should be rejected
};

/// Result of validating a LaunchProfile.
struct ValidationResult {
    bool valid = true;                         ///< False if any critical errors
    std::vector<ValidationError> errors;       ///< All collected errors
    std::vector<size_t> invalid_entry_indices; ///< Entries that should be skipped
};

/// Validates a LaunchProfile for correctness.
/// Collects ALL errors before returning (does not stop at first error).
class ConfigValidator {
public:
    /// Validate a loaded profile. Returns all errors found.
    ValidationResult validate(const LaunchProfile& profile,
                              const std::filesystem::path& source_file) const;

private:
    void validateProfileFields(const LaunchProfile& profile,
                               ValidationResult& result) const;

    void validateEntry(const LaunchEntry& entry, size_t index,
                       const std::vector<LaunchEntry>& all_entries,
                       ValidationResult& result) const;

    void validateMonitoredTopics(const LaunchProfile& profile,
                                 ValidationResult& result) const;

    void validateLogConfig(const LogConfig& config,
                           ValidationResult& result) const;

    /// Check if a string is a valid ROS 2 parameter name.
    /// Valid: [a-zA-Z_][a-zA-Z0-9_./]*
    static bool isValidRosParamName(const std::string& name);

    /// Check if a string is a valid environment variable name.
    /// Valid: [a-zA-Z_][a-zA-Z0-9_]*
    static bool isValidEnvVarName(const std::string& name);
};

}  // namespace rtl
