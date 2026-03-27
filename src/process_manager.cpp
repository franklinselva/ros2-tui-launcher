#include "ros2_tui_launcher/process_manager.hpp"

#include <spdlog/spdlog.h>

#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>

namespace rtl {

const char* processStateStr(ProcessState s) {
    switch (s) {
        case ProcessState::Stopped:  return "Stopped";
        case ProcessState::Starting: return "Starting";
        case ProcessState::Running:  return "Running";
        case ProcessState::Crashed:  return "Crashed";
        case ProcessState::Stopping: return "Stopping";
    }
    return "Unknown";
}

struct ProcessManager::ManagedProcess {
    LaunchEntry entry;
    ProcessInfo info;
    int stdout_fd = -1;
    int stderr_fd = -1;
    std::thread reader_thread;
    std::thread wait_thread;
    std::atomic<bool> active{false};
};

ProcessManager::ProcessManager() = default;

ProcessManager::~ProcessManager() {
    stopAll();
}

void ProcessManager::setLogCallback(LogCallback cb) {
    std::lock_guard lock(mutex_);
    log_callback_ = std::move(cb);
}

bool ProcessManager::start(const LaunchEntry& entry) {
    std::string name = entry.displayName();

    {
        std::lock_guard lock(mutex_);
        if (procs_.count(name) && procs_[name]->active.load()) {
            spdlog::warn("Process '{}' is already running", name);
            return false;
        }
    }

    // Build command
    std::vector<std::string> argv;
    if (!entry.launch_file.empty()) {
        argv = {"ros2", "launch", entry.package, entry.launch_file};
        for (const auto& [k, v] : entry.args) {
            argv.push_back(k + ":=" + v);
        }
    } else {
        argv = {"ros2", "run", entry.package, entry.executable};
        for (const auto& [k, v] : entry.args) {
            argv.push_back("--ros-args");
            argv.push_back("-p");
            argv.push_back(k + ":=" + v);
        }
    }

    // Create pipes with O_CLOEXEC to prevent FD leaks into child processes
    int stdout_pipe[2], stderr_pipe[2];
    if (pipe2(stdout_pipe, O_CLOEXEC) != 0) {
        spdlog::error("Failed to create stdout pipe for '{}': {}", name, strerror(errno));
        return false;
    }
    if (pipe2(stderr_pipe, O_CLOEXEC) != 0) {
        spdlog::error("Failed to create stderr pipe for '{}': {}", name, strerror(errno));
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        spdlog::error("Fork failed for '{}': {}", name, strerror(errno));
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        return false;
    }

    if (pid == 0) {
        // Child process — dup2 clears O_CLOEXEC on the target FDs
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        // Set environment variables
        for (const auto& [k, v] : entry.env) {
            setenv(k.c_str(), v.c_str(), 1);
        }

        // Build exec args
        std::vector<char*> c_argv;
        for (auto& a : argv) {
            c_argv.push_back(a.data());
        }
        c_argv.push_back(nullptr);

        // Create new process group so we can signal the whole group
        setpgid(0, 0);

        execvp(c_argv[0], c_argv.data());
        // If we get here, exec failed
        _exit(127);
    }

    // Parent process
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    auto proc = std::make_shared<ManagedProcess>();
    proc->entry = entry;
    proc->info.name = name;
    proc->info.pid = pid;
    proc->info.state.store(ProcessState::Running);
    proc->info.started_at = std::chrono::steady_clock::now();
    proc->info.restart_policy = entry.restart_policy;
    proc->stdout_fd = stdout_pipe[0];
    proc->stderr_fd = stderr_pipe[0];
    proc->active.store(true);

    // Start reader thread (reads stdout and stderr)
    proc->reader_thread = std::thread(&ProcessManager::readerThread, this, proc);

    // Start wait thread (waits for process exit)
    proc->wait_thread = std::thread(&ProcessManager::waitThread, this, proc);

    {
        std::lock_guard lock(mutex_);
        procs_[name] = proc;
    }

    spdlog::info("Started process '{}' (pid={})", name, pid);
    return true;
}

void ProcessManager::stop(const std::string& name, std::chrono::milliseconds timeout) {
    std::shared_ptr<ManagedProcess> proc;
    {
        std::lock_guard lock(mutex_);
        auto it = procs_.find(name);
        if (it == procs_.end()) return;
        proc = it->second;
    }

    if (!proc->active.load()) return;

    proc->info.state.store(ProcessState::Stopping);

    // Send SIGINT to the process group
    if (proc->info.pid > 0) {
        kill(-proc->info.pid, SIGINT);
    }

    // Wait for graceful shutdown
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (proc->active.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Force kill if still running
    if (proc->active.load() && proc->info.pid > 0) {
        spdlog::warn("Force killing '{}'", name);
        kill(-proc->info.pid, SIGKILL);
    }

    // Wait for threads to finish
    if (proc->wait_thread.joinable()) proc->wait_thread.join();
    if (proc->reader_thread.joinable()) proc->reader_thread.join();
}

void ProcessManager::stopAll() {
    std::vector<std::string> names;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [name, _] : procs_) {
            names.push_back(name);
        }
    }
    for (const auto& name : names) {
        stop(name);
    }
}

void ProcessManager::restart(const std::string& name) {
    LaunchEntry entry;
    {
        std::lock_guard lock(mutex_);
        auto it = procs_.find(name);
        if (it == procs_.end()) return;
        entry = it->second->entry;
    }
    stop(name);
    start(entry);
}

std::vector<ProcessInfo> ProcessManager::processes() const {
    std::lock_guard lock(mutex_);
    std::vector<ProcessInfo> result;
    result.reserve(procs_.size());
    for (const auto& [_, proc] : procs_) {
        result.push_back(proc->info);
    }
    return result;
}

ProcessInfo ProcessManager::processInfo(const std::string& name) const {
    std::lock_guard lock(mutex_);
    auto it = procs_.find(name);
    if (it == procs_.end()) return {};
    return it->second->info;
}

void ProcessManager::readerThread(std::shared_ptr<ManagedProcess> proc) {
    auto readFd = [&](int fd) {
        std::array<char, 4096> buf;
        std::string line_buffer;
        size_t search_start = 0;

        while (proc->active.load()) {
            ssize_t n = read(fd, buf.data(), buf.size());
            if (n <= 0) break;

            line_buffer.append(buf.data(), static_cast<size_t>(n));

            // Process complete lines efficiently — track search position
            size_t pos;
            while ((pos = line_buffer.find('\n', search_start)) != std::string::npos) {
                std::string line = line_buffer.substr(0, pos);

                std::lock_guard lock(mutex_);
                if (log_callback_) {
                    log_callback_(proc->info.name, line);
                }

                line_buffer.erase(0, pos + 1);
                search_start = 0;
            }
            search_start = line_buffer.size();
        }

        // Flush remaining
        if (!line_buffer.empty()) {
            std::lock_guard lock(mutex_);
            if (log_callback_) {
                log_callback_(proc->info.name, line_buffer);
            }
        }

        close(fd);
    };

    // Read both stdout and stderr in this thread using a simple approach:
    // We read stdout in this thread and stderr in a nested thread
    std::thread stderr_reader([&] { readFd(proc->stderr_fd); });
    readFd(proc->stdout_fd);
    stderr_reader.join();
}

void ProcessManager::waitThread(std::shared_ptr<ManagedProcess> proc) {
    int status = 0;
    waitpid(proc->info.pid, &status, 0);

    proc->active.store(false);

    if (WIFEXITED(status)) {
        proc->info.exit_code.store(WEXITSTATUS(status));
        if (proc->info.state.load() != ProcessState::Stopping) {
            proc->info.state.store((proc->info.exit_code.load() == 0)
                ? ProcessState::Stopped
                : ProcessState::Crashed);
        } else {
            proc->info.state.store(ProcessState::Stopped);
        }
    } else if (WIFSIGNALED(status)) {
        proc->info.exit_code.store(-WTERMSIG(status));
        proc->info.state.store((proc->info.state.load() == ProcessState::Stopping)
            ? ProcessState::Stopped
            : ProcessState::Crashed);
    }

    spdlog::info("Process '{}' exited (code={})", proc->info.name, proc->info.exit_code.load());

    // Handle restart policy — actually perform the restart
    if (proc->info.state.load() == ProcessState::Crashed) {
        if (proc->entry.restart_policy == "always" ||
            proc->entry.restart_policy == "on-failure") {
            spdlog::info("Restarting '{}' per restart_policy='{}'",
                         proc->info.name, proc->entry.restart_policy);
            // Detach a thread to avoid blocking waitThread while restart runs stop+start
            std::thread([this, entry = proc->entry]() {
                start(entry);
            }).detach();
        }
    } else if (proc->entry.restart_policy == "always") {
        spdlog::info("Restarting '{}' per restart_policy='always'",
                     proc->info.name);
        std::thread([this, entry = proc->entry]() {
            start(entry);
        }).detach();
    }
}

}  // namespace rtl
