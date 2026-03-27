#pragma once

#include <rclcpp/rclcpp.hpp>
#include <rcl_interfaces/msg/log.hpp>

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace rtl {

// Forward declaration
class LogWriter;

/// Severity level (mirrors rcl_interfaces::msg::Log levels).
enum class LogLevel {
    Debug = 10,
    Info = 20,
    Warn = 30,
    Error = 40,
    Fatal = 50,
    Unknown = 0,
};

const char* logLevelStr(LogLevel l);
LogLevel fromRclLevel(uint8_t level);

/// A single log entry.
struct LogEntry {
    std::string source;      ///< Node name
    std::string message;     ///< Clean log message (redundant prefixes stripped)
    LogLevel level = LogLevel::Unknown;
    std::chrono::system_clock::time_point wall_time;  ///< Wall-clock time for display
};

/// Format a wall-clock time_point as "HH:MM:SS.mmm".
std::string formatTimestamp(const std::chrono::system_clock::time_point& tp);

/// Aggregates logs from two sources:
/// 1. /rosout subscription (structured ROS 2 logs)
/// 2. Process stdout/stderr (raw output from launched processes)
class LogAggregator {
public:
    /// @param node  rclcpp node used to subscribe to /rosout
    /// @param max_lines  Maximum log entries to keep in buffer
    explicit LogAggregator(rclcpp::Node::SharedPtr node, size_t max_lines = 10000);

    /// Push a raw log line from a child process stdout/stderr.
    /// Parses and strips the ROS 2 console format:
    ///   [LEVEL] [timestamp] [node_name]: message
    /// into structured fields (level, source, clean message).
    void pushRaw(const std::string& source, const std::string& message);

    /// Get filtered log entries.
    std::vector<LogEntry> filtered(
        const std::string& source_filter = "",
        LogLevel min_level = LogLevel::Debug,
        const std::string& search = "") const;

    /// Get all entries (no filter).
    std::vector<LogEntry> all() const;

    /// Get list of known sources.
    std::vector<std::string> sources() const;

    /// Total entry count.
    size_t size() const;

    /// Generation counter — incremented on every mutation.
    /// Callers can compare to skip redundant filtered() calls.
    uint64_t generation() const;

    /// Clear all entries.
    void clear();

    /// Set the log writer for file persistence.
    /// Thread-safe. Pass nullptr to disable persistence.
    void setLogWriter(std::shared_ptr<LogWriter> writer);

private:
    void rosoutCallback(const rcl_interfaces::msg::Log::SharedPtr msg);

    rclcpp::Node::SharedPtr node_;
    rclcpp::Subscription<rcl_interfaces::msg::Log>::SharedPtr rosout_sub_;
    mutable std::mutex mutex_;
    std::deque<LogEntry> entries_;
    std::set<std::string> known_sources_;
    size_t max_lines_;
    uint64_t generation_ = 0;

    std::shared_ptr<LogWriter> log_writer_;
    std::mutex writer_mutex_;  ///< Separate mutex for log writer access
};

}  // namespace rtl
