#pragma once

#include "ros2_tui_launcher/launch_profile.hpp"
#include "ros2_tui_launcher/log_aggregator.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace rtl {

/// Writes log entries to per-process files with rotation support.
///
/// Directory layout:
///   <log_dir>/<session_timestamp>/<profile_name>/<process>.log
///
/// Thread-safe: write() and writeRosout() can be called from any thread.
/// Uses buffered FILE* with 64KB setvbuf for efficient batched I/O.
class LogWriter {
public:
    /// @param config     Log configuration (dir, rotation, flush interval)
    /// @param profile_name  Active profile name (used as subdirectory)
    /// @param session_ts    Session timestamp string (e.g. "2026-03-27_14-30-12")
    LogWriter(const LogConfig& config,
              const std::string& profile_name,
              const std::string& session_ts);
    ~LogWriter();

    LogWriter(const LogWriter&) = delete;
    LogWriter& operator=(const LogWriter&) = delete;

    /// Write a log entry to the process-specific log file.
    /// @param process_name  Used as the filename (sanitized)
    /// @param entry         The log entry to write
    void write(const std::string& process_name, const LogEntry& entry);

    /// Write a log entry to the rosout.log file.
    void writeRosout(const LogEntry& entry);

    /// Flush all open file buffers to disk.
    void flushAll();

    /// Close all files. Safe to call multiple times.
    void close();

    /// Whether this writer successfully initialized (log dir created).
    bool active() const { return active_; }

    /// Generate a session timestamp string for the current time.
    static std::string sessionTimestamp();

private:
    struct FileState {
        FILE* fp = nullptr;
        size_t current_size = 0;
        std::string base_path;  // full path without rotation suffix
    };

    /// Get or open a file for the given source name.
    FileState& getOrOpen(const std::string& name);

    /// Rotate a log file when it exceeds max size.
    void rotate(FileState& fs);

    /// Write a formatted line to a FileState.
    void writeLine(FileState& fs, const LogEntry& entry);

    /// Sanitize a name for use as a filename.
    static std::string sanitize(const std::string& name);

    LogConfig config_;
    std::filesystem::path log_dir_;  // fully resolved: <base>/<timestamp>/<profile>/
    bool active_ = false;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, FileState> files_;

    // Background flush thread
    std::thread flush_thread_;
    std::atomic<bool> running_{false};
    std::condition_variable flush_cv_;
    std::mutex flush_cv_mutex_;
};

}  // namespace rtl
