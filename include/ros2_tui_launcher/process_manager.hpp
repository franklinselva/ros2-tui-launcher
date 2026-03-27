#pragma once

#include "ros2_tui_launcher/launch_profile.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rtl {

/// State of a managed process.
enum class ProcessState {
    Stopped,
    Starting,
    Running,
    Crashed,
    Stopping,
};

const char* processStateStr(ProcessState s);

/// Information about a managed process.
struct ProcessInfo {
    std::string name;
    pid_t pid = -1;
    std::atomic<ProcessState> state{ProcessState::Stopped};
    std::atomic<int> exit_code{0};
    std::chrono::steady_clock::time_point started_at;
    std::string restart_policy;

    ProcessInfo() = default;
    ProcessInfo(const ProcessInfo& o)
        : name(o.name), pid(o.pid)
        , state(o.state.load()), exit_code(o.exit_code.load())
        , started_at(o.started_at), restart_policy(o.restart_policy) {}
    ProcessInfo& operator=(const ProcessInfo& o) {
        if (this != &o) {
            name = o.name;
            pid = o.pid;
            state.store(o.state.load());
            exit_code.store(o.exit_code.load());
            started_at = o.started_at;
            restart_policy = o.restart_policy;
        }
        return *this;
    }
};

/// Callback for log lines from child processes.
/// (process_name, line)
using LogCallback = std::function<void(const std::string&, const std::string&)>;

/// Manages child processes for launch entries.
/// Spawns ros2 launch/run as subprocesses, captures their output,
/// and tracks their lifecycle.
class ProcessManager {
public:
    ProcessManager();
    ~ProcessManager();

    ProcessManager(const ProcessManager&) = delete;
    ProcessManager& operator=(const ProcessManager&) = delete;

    /// Set the callback that receives log lines from child processes.
    void setLogCallback(LogCallback cb);

    /// Start a launch entry. Returns true if started successfully.
    bool start(const LaunchEntry& entry);

    /// Stop a process by name. Sends SIGINT, then SIGKILL after timeout.
    void stop(const std::string& name, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    /// Stop all managed processes.
    void stopAll();

    /// Restart a process by name.
    void restart(const std::string& name);

    /// Get info about all managed processes.
    std::vector<ProcessInfo> processes() const;

    /// Get info about a specific process.
    ProcessInfo processInfo(const std::string& name) const;

private:
    struct ManagedProcess;

    void readerThread(std::shared_ptr<ManagedProcess> proc);
    void waitThread(std::shared_ptr<ManagedProcess> proc);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<ManagedProcess>> procs_;
    LogCallback log_callback_;
};

}  // namespace rtl
