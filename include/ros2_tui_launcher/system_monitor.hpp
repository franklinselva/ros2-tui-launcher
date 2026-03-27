#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/types.h>

namespace rtl {

/// System-wide hardware and utilization info.
struct SystemInfo {
    std::string cpu_model;
    int cpu_cores = 0;          ///< Physical cores
    int cpu_threads = 0;        ///< Logical threads (from libproc2)
    unsigned long mem_total_kb = 0;
    unsigned long mem_used_kb = 0;
    unsigned long mem_available_kb = 0;
    double cpu_usage_percent = 0.0;  ///< System-wide CPU%

    // GPU (optional — empty if no nvidia-smi)
    std::string gpu_name;
    unsigned long gpu_mem_total_mb = 0;
    unsigned long gpu_mem_used_mb = 0;
    double gpu_utilization = 0.0;
    double gpu_temp_c = 0.0;
    bool has_gpu = false;
};

/// Per-process stats from /proc.
struct ProcessStats {
    pid_t pid = 0;
    pid_t ppid = 0;
    std::string comm;
    double cpu_percent = 0.0;
    unsigned long mem_rss_kb = 0;
    unsigned long gpu_mem_mb = 0;   ///< Per-process GPU mem (from nvidia-smi)
    char state = '?';
};

/// A node in the process tree rooted at a managed PID.
struct ProcessTreeNode {
    ProcessStats stats;
    std::vector<ProcessTreeNode> children;
    // Aggregated totals (self + all descendants)
    double total_cpu_percent = 0.0;
    unsigned long total_mem_rss_kb = 0;
    unsigned long total_gpu_mem_mb = 0;
};

/// Monitors system resources and per-process stats using libproc2.
/// Thread-safe: all public methods can be called from any thread.
class SystemMonitor {
public:
    SystemMonitor();
    ~SystemMonitor();

    SystemMonitor(const SystemMonitor&) = delete;
    SystemMonitor& operator=(const SystemMonitor&) = delete;

    /// Refresh all stats. Throttled internally.
    /// Safe to call at 30 Hz — actual work happens at ~2 Hz.
    void refresh();

    /// Get cached system-wide info.
    SystemInfo systemInfo() const;

    /// Build a process tree rooted at the given PID.
    /// Recursively collects children via PPID and aggregates stats.
    ProcessTreeNode processTree(pid_t root_pid) const;

    /// Get stats for a single PID (no tree).
    ProcessStats processStats(pid_t pid) const;

private:
    void detectSystemInfo();
    void refreshProcesses();
    void refreshSystemCpu();
    void refreshSystemMem();
    void refreshGpu();

    /// Recursively build tree from cached_procs_.
    ProcessTreeNode buildTree(pid_t pid) const;

    // libproc2 opaque handles (forward-declared as void* to avoid C header in hpp)
    void* pids_handle_ = nullptr;
    void* stat_handle_ = nullptr;
    void* mem_handle_ = nullptr;

    mutable std::mutex mutex_;
    SystemInfo cached_system_;
    std::unordered_map<pid_t, ProcessStats> cached_procs_;

    // Per-process GPU memory from nvidia-smi
    std::unordered_map<pid_t, unsigned long> gpu_proc_mem_;

    // CPU% delta tracking
    unsigned long long prev_total_ticks_ = 0;
    unsigned long long prev_busy_ticks_ = 0;
    unsigned long long delta_total_ticks_ = 0;  ///< Shared between refreshSystemCpu and refreshProcesses
    std::unordered_map<pid_t, unsigned long long> prev_proc_ticks_;
    long hertz_ = 100;
    long num_cpus_ = 1;

    // Throttle
    std::chrono::steady_clock::time_point last_proc_refresh_{};
    std::chrono::steady_clock::time_point last_gpu_refresh_{};
    static constexpr std::chrono::milliseconds kProcInterval{500};
    static constexpr std::chrono::milliseconds kGpuInterval{1000};
};

}  // namespace rtl
