#include "ros2_tui_launcher/log_writer.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <ctime>

namespace rtl {

std::string LogWriter::sessionTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_val = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&time_t_val, &tm_buf);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d_%02d-%02d-%02d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return buf;
}

std::string LogWriter::sanitize(const std::string& name) {
    std::string result;
    result.reserve(name.size());
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            result += c;
        } else {
            result += '_';
        }
    }
    // Avoid empty filenames
    if (result.empty()) result = "unnamed";
    return result;
}

LogWriter::LogWriter(const LogConfig& config,
                     const std::string& profile_name,
                     const std::string& session_ts)
    : config_(config)
{
    // Resolve base log directory
    std::filesystem::path base_dir;
    if (config_.log_dir.empty()) {
        base_dir = std::filesystem::current_path() / "log";
    } else {
        base_dir = config_.log_dir;
    }

    // Build full path: <base>/<timestamp>/<profile>/
    log_dir_ = base_dir / session_ts / sanitize(profile_name);

    try {
        std::filesystem::create_directories(log_dir_);
        active_ = true;
        spdlog::info("Log directory: {}", log_dir_.string());
    } catch (const std::exception& e) {
        spdlog::warn("Failed to create log directory '{}': {}. "
                     "Log persistence disabled.", log_dir_.string(), e.what());
        return;
    }

    // Start background flush thread
    running_.store(true);
    flush_thread_ = std::thread([this] {
        while (running_.load()) {
            {
                std::unique_lock lock(flush_cv_mutex_);
                flush_cv_.wait_for(lock, config_.flush_interval,
                    [this] { return !running_.load(); });
            }
            if (running_.load()) {
                flushAll();
            }
        }
    });
}

LogWriter::~LogWriter() {
    running_.store(false);
    flush_cv_.notify_one();
    if (flush_thread_.joinable()) {
        flush_thread_.join();
    }
    close();
}

LogWriter::FileState& LogWriter::getOrOpen(const std::string& name) {
    auto it = files_.find(name);
    if (it != files_.end() && it->second.fp) {
        return it->second;
    }

    FileState fs;
    fs.base_path = (log_dir_ / (sanitize(name) + ".log")).string();
    fs.fp = std::fopen(fs.base_path.c_str(), "a");
    if (!fs.fp) {
        spdlog::warn("Failed to open log file '{}': {}", fs.base_path, strerror(errno));
        // Insert a dead entry so we don't retry every write
        files_[name] = fs;
        return files_[name];
    }

    // 64KB fully-buffered I/O for efficient batched writes
    std::setvbuf(fs.fp, nullptr, _IOFBF, 65536);

    // Track current size for rotation (handles appending to existing files)
    std::fseek(fs.fp, 0, SEEK_END);
    fs.current_size = static_cast<size_t>(std::ftell(fs.fp));

    files_[name] = std::move(fs);
    return files_[name];
}

void LogWriter::writeLine(FileState& fs, const LogEntry& entry) {
    if (!fs.fp) return;

    // Format timestamp
    auto time_t_val = std::chrono::system_clock::to_time_t(entry.wall_time);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        entry.wall_time.time_since_epoch()) % 1000;
    std::tm tm_buf{};
    localtime_r(&time_t_val, &tm_buf);

    // Format: [YYYY-MM-DD HH:MM:SS.mmm] [LEVEL] [SOURCE] MESSAGE\n
    // Use stack buffer for common case, fallback to dynamic for long messages
    char ts_buf[48];
    std::snprintf(ts_buf, sizeof(ts_buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                  static_cast<int>(ms.count()));

    const char* level = logLevelStr(entry.level);

    // Try stack buffer first (covers most log lines)
    char stack_buf[512];
    int n = std::snprintf(stack_buf, sizeof(stack_buf), "[%s] [%s] [%s] %s\n",
                          ts_buf, level, entry.source.c_str(),
                          entry.message.c_str());

    if (n > 0 && static_cast<size_t>(n) < sizeof(stack_buf)) {
        size_t written = std::fwrite(stack_buf, 1, static_cast<size_t>(n), fs.fp);
        fs.current_size += written;
    } else if (n > 0) {
        // Line too long for stack buffer — allocate
        std::string line(static_cast<size_t>(n) + 1, '\0');
        std::snprintf(line.data(), line.size(), "[%s] [%s] [%s] %s\n",
                      ts_buf, level, entry.source.c_str(),
                      entry.message.c_str());
        size_t written = std::fwrite(line.data(), 1, static_cast<size_t>(n), fs.fp);
        fs.current_size += written;
    }

    // Check rotation
    if (config_.max_file_size_bytes > 0 &&
        fs.current_size >= config_.max_file_size_bytes) {
        rotate(fs);
    }
}

void LogWriter::rotate(FileState& fs) {
    if (!fs.fp) return;
    std::fclose(fs.fp);
    fs.fp = nullptr;

    // Rotate: file.log.N → file.log.N+1, delete oldest
    for (int i = static_cast<int>(config_.max_rotated_files) - 1; i >= 1; --i) {
        std::string src = fs.base_path + "." + std::to_string(i);
        std::string dst = fs.base_path + "." + std::to_string(i + 1);
        std::error_code ec;
        std::filesystem::rename(src, dst, ec);  // ok if src doesn't exist
    }

    // Current → .1
    {
        std::error_code ec;
        std::filesystem::rename(fs.base_path, fs.base_path + ".1", ec);
    }

    // Delete oldest if it exceeds max
    {
        std::string oldest = fs.base_path + "." +
            std::to_string(config_.max_rotated_files + 1);
        std::error_code ec;
        std::filesystem::remove(oldest, ec);
    }

    // Reopen fresh
    fs.fp = std::fopen(fs.base_path.c_str(), "w");
    if (fs.fp) {
        std::setvbuf(fs.fp, nullptr, _IOFBF, 65536);
    }
    fs.current_size = 0;
}

void LogWriter::write(const std::string& process_name, const LogEntry& entry) {
    if (!active_) return;
    std::lock_guard lock(mutex_);
    auto& fs = getOrOpen(process_name);
    writeLine(fs, entry);
}

void LogWriter::writeRosout(const LogEntry& entry) {
    if (!active_) return;
    std::lock_guard lock(mutex_);
    auto& fs = getOrOpen("rosout");
    writeLine(fs, entry);
}

void LogWriter::flushAll() {
    std::lock_guard lock(mutex_);
    for (auto& [_, fs] : files_) {
        if (fs.fp) {
            std::fflush(fs.fp);
        }
    }
}

void LogWriter::close() {
    std::lock_guard lock(mutex_);
    for (auto& [_, fs] : files_) {
        if (fs.fp) {
            std::fflush(fs.fp);
            std::fclose(fs.fp);
            fs.fp = nullptr;
        }
    }
    files_.clear();
    active_ = false;
}

}  // namespace rtl
