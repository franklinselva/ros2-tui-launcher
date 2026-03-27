#include "ros2_tui_launcher/log_aggregator.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <set>

namespace rtl {

const char* logLevelStr(LogLevel l) {
    switch (l) {
        case LogLevel::Debug:   return "DEBUG";
        case LogLevel::Info:    return " INFO";
        case LogLevel::Warn:    return " WARN";
        case LogLevel::Error:   return "ERROR";
        case LogLevel::Fatal:   return "FATAL";
        case LogLevel::Unknown: return "  ???";
    }
    return "  ???";
}

LogLevel fromRclLevel(uint8_t level) {
    if (level >= 50) return LogLevel::Fatal;
    if (level >= 40) return LogLevel::Error;
    if (level >= 30) return LogLevel::Warn;
    if (level >= 20) return LogLevel::Info;
    if (level >= 10) return LogLevel::Debug;
    return LogLevel::Unknown;
}

std::string formatTimestamp(const std::chrono::system_clock::time_point& tp) {
    auto time_t_val = std::chrono::system_clock::to_time_t(tp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        tp.time_since_epoch()) % 1000;

    std::tm tm_buf{};
    localtime_r(&time_t_val, &tm_buf);

    char buf[16];  // "HH:MM:SS.mmm\0" = 13 chars
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                  static_cast<int>(ms.count()));
    return buf;
}

LogAggregator::LogAggregator(rclcpp::Node::SharedPtr node, size_t max_lines)
    : node_(node), max_lines_(max_lines)
{
    rosout_sub_ = node_->create_subscription<rcl_interfaces::msg::Log>(
        "/rosout", rclcpp::QoS(100).best_effort(),
        [this](const rcl_interfaces::msg::Log::SharedPtr msg) {
            rosoutCallback(msg);
        });
}

void LogAggregator::rosoutCallback(const rcl_interfaces::msg::Log::SharedPtr msg) {
    LogEntry entry;
    entry.source = msg->name;
    entry.message = msg->msg;
    entry.level = fromRclLevel(msg->level);

    // Use the ROS message stamp for wall time
    auto sec = std::chrono::seconds(msg->stamp.sec);
    auto nsec = std::chrono::nanoseconds(msg->stamp.nanosec);
    entry.wall_time = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(sec + nsec));

    std::lock_guard lock(mutex_);
    known_sources_.insert(entry.source);
    entries_.push_back(std::move(entry));
    while (entries_.size() > max_lines_) {
        entries_.pop_front();
    }
    ++generation_;
}

/// Parse and strip ROS 2 console log format from raw stdout lines.
///
/// Input formats:
///   [INFO] [1711234567.890123456] [talker]: Publishing: 'Hello World: 1'
///   [WARNING] [1711234567.890] [/ns/node]: Some warning
///   Plain text without ROS format
///
/// Extracts: level, node name (overrides source if found), clean message.
void LogAggregator::pushRaw(const std::string& source, const std::string& message) {
    LogEntry entry;
    entry.source = source;
    entry.wall_time = std::chrono::system_clock::now();
    entry.level = LogLevel::Info;

    std::string_view line(message);

    // Try to parse ROS 2 console format: [LEVEL] [timestamp] [node]: message
    // Step 1: Parse [LEVEL]
    if (!line.empty() && line[0] == '[') {
        auto close = line.find(']', 1);
        if (close != std::string_view::npos) {
            auto level_str = line.substr(1, close - 1);

            LogLevel parsed_level = LogLevel::Unknown;
            if (level_str == "DEBUG")        parsed_level = LogLevel::Debug;
            else if (level_str == "INFO")    parsed_level = LogLevel::Info;
            else if (level_str == "WARN" || level_str == "WARNING")
                                             parsed_level = LogLevel::Warn;
            else if (level_str == "ERROR")   parsed_level = LogLevel::Error;
            else if (level_str == "FATAL")   parsed_level = LogLevel::Fatal;

            if (parsed_level != LogLevel::Unknown) {
                entry.level = parsed_level;
                line = line.substr(close + 1);

                // Skip whitespace
                while (!line.empty() && line[0] == ' ') line = line.substr(1);

                // Step 2: Parse [timestamp] — skip it entirely
                if (!line.empty() && line[0] == '[') {
                    auto ts_close = line.find(']', 1);
                    if (ts_close != std::string_view::npos) {
                        line = line.substr(ts_close + 1);
                        while (!line.empty() && line[0] == ' ') line = line.substr(1);
                    }
                }

                // Step 3: Parse [node_name]: — strip it since we have source column
                if (!line.empty() && line[0] == '[') {
                    auto node_close = line.find(']', 1);
                    if (node_close != std::string_view::npos) {
                        auto node_name = line.substr(1, node_close - 1);
                        // Use parsed node name as source (more specific than process name)
                        entry.source = std::string(node_name);
                        line = line.substr(node_close + 1);

                        // Skip ": " after node name
                        if (line.size() >= 2 && line[0] == ':' && line[1] == ' ') {
                            line = line.substr(2);
                        } else if (!line.empty() && line[0] == ':') {
                            line = line.substr(1);
                        }
                        // Skip leading whitespace
                        while (!line.empty() && line[0] == ' ') line = line.substr(1);
                    }
                }
            }
        }
    }

    entry.message = std::string(line);

    std::lock_guard lock(mutex_);
    known_sources_.insert(entry.source);
    entries_.push_back(std::move(entry));
    while (entries_.size() > max_lines_) {
        entries_.pop_front();
    }
    ++generation_;
}

std::vector<LogEntry> LogAggregator::filtered(
    const std::string& source_filter,
    LogLevel min_level,
    const std::string& search) const
{
    std::lock_guard lock(mutex_);
    std::vector<LogEntry> result;
    result.reserve(entries_.size());

    for (const auto& e : entries_) {
        if (!source_filter.empty() && e.source != source_filter) continue;
        if (static_cast<int>(e.level) < static_cast<int>(min_level)) continue;
        if (!search.empty() && e.message.find(search) == std::string::npos) continue;
        result.push_back(e);
    }

    return result;
}

std::vector<LogEntry> LogAggregator::all() const {
    std::lock_guard lock(mutex_);
    return {entries_.begin(), entries_.end()};
}

std::vector<std::string> LogAggregator::sources() const {
    std::lock_guard lock(mutex_);
    return {known_sources_.begin(), known_sources_.end()};
}

size_t LogAggregator::size() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

uint64_t LogAggregator::generation() const {
    std::lock_guard lock(mutex_);
    return generation_;
}

void LogAggregator::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
    known_sources_.clear();
    ++generation_;
}

}  // namespace rtl
