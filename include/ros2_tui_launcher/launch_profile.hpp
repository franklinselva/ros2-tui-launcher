#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace rtl {

/// Configuration for log file persistence.
struct LogConfig {
    std::string log_dir;                               ///< Empty = "<cwd>/log/"
    size_t max_file_size_bytes = 10 * 1024 * 1024;     ///< 10 MB default
    size_t max_rotated_files = 5;                      ///< Keep 5 rotated files
    std::chrono::milliseconds flush_interval{1000};    ///< 1 second flush
};

/// A single launch entry within a profile.
struct LaunchEntry {
    std::string name;       ///< Display name (auto-derived if empty)
    std::string package;    ///< ROS 2 package name
    std::string launch_file;///< Launch file name (e.g. "teleop.launch.py")
    std::string executable; ///< Alternative: run a single node executable

    /// Launch arguments (key=value pairs passed to ros2 launch)
    std::unordered_map<std::string, std::string> args;

    /// Environment variable overrides
    std::unordered_map<std::string, std::string> env;

    /// Whether to start this entry automatically when the profile loads
    bool autostart = true;

    /// Restart policy: "never", "on-failure", "always"
    std::string restart_policy = "never";

    /// Path to a ROS 2 parameter YAML file (passed via --params-file)
    std::string params_file;

    /// Inline ROS 2 node parameters (passed via -p key:=value)
    std::unordered_map<std::string, std::string> parameters;

    /// Display name, derived from package/launch_file if not set
    std::string displayName() const;
};

/// Monitored topic configuration.
struct MonitoredTopic {
    std::string topic;
    double expected_hz = 0.0;  ///< 0 = no expectation, just display
};

/// A launch profile loaded from YAML.
struct LaunchProfile {
    std::string name;
    std::string description;
    std::vector<LaunchEntry> entries;
    std::vector<MonitoredTopic> monitored_topics;
    double topic_hz_rate = 1.0;  ///< How often to poll topic frequencies
    LogConfig log_config;        ///< Log persistence configuration
};

/// Load a single profile from a YAML file.
LaunchProfile loadProfile(const std::filesystem::path& path);

/// Discover all .yaml profiles in a directory.
std::vector<LaunchProfile> discoverProfiles(const std::filesystem::path& dir);

}  // namespace rtl
