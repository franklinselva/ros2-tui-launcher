#include "ros2_tui_launcher/system_monitor.hpp"

#include <libproc2/pids.h>
#include <libproc2/stat.h>
#include <libproc2/meminfo.h>
#include <libproc2/misc.h>

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace rtl {

// Items we query from libproc2 for each process
enum PidsIdx {
    PI_PID = 0,
    PI_PPID,
    PI_CMD,
    PI_TICS_ALL,
    PI_VM_RSS,
    PI_STATE,
    PI_COUNT
};

static enum pids_item pids_items[] = {
    PIDS_ID_PID,       // s_int
    PIDS_ID_PPID,      // s_int
    PIDS_CMD,          // str
    PIDS_TICS_ALL,     // ull_int
    PIDS_VM_RSS,       // ul_int  (KiB)
    PIDS_STATE,        // s_ch
};

SystemMonitor::SystemMonitor() {
    hertz_ = procps_hertz_get();
    num_cpus_ = procps_cpu_count();
    if (hertz_ <= 0) hertz_ = 100;
    if (num_cpus_ <= 0) num_cpus_ = 1;

    // Init libproc2 handles
    if (procps_pids_new(reinterpret_cast<struct pids_info**>(&pids_handle_),
                        pids_items, PI_COUNT) < 0) {
        spdlog::error("SystemMonitor: failed to init pids");
        pids_handle_ = nullptr;
    }

    if (procps_stat_new(reinterpret_cast<struct stat_info**>(&stat_handle_)) < 0) {
        spdlog::error("SystemMonitor: failed to init stat");
        stat_handle_ = nullptr;
    }

    if (procps_meminfo_new(reinterpret_cast<struct meminfo_info**>(&mem_handle_)) < 0) {
        spdlog::error("SystemMonitor: failed to init meminfo");
        mem_handle_ = nullptr;
    }

    detectSystemInfo();
}

SystemMonitor::~SystemMonitor() {
    if (pids_handle_) {
        auto p = reinterpret_cast<struct pids_info**>(&pids_handle_);
        procps_pids_unref(p);
    }
    if (stat_handle_) {
        auto p = reinterpret_cast<struct stat_info**>(&stat_handle_);
        procps_stat_unref(p);
    }
    if (mem_handle_) {
        auto p = reinterpret_cast<struct meminfo_info**>(&mem_handle_);
        procps_meminfo_unref(p);
    }
}

void SystemMonitor::detectSystemInfo() {
    // CPU model from /proc/cpuinfo
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    int physical_ids = 0;
    std::string model;
    while (std::getline(cpuinfo, line)) {
        if (line.find("model name") == 0) {
            auto pos = line.find(':');
            if (pos != std::string::npos) {
                model = line.substr(pos + 2);
            }
        }
        if (line.find("cpu cores") == 0) {
            auto pos = line.find(':');
            if (pos != std::string::npos) {
                try { physical_ids = std::stoi(line.substr(pos + 2)); } catch (...) {}
            }
        }
    }

    std::lock_guard lock(mutex_);
    cached_system_.cpu_model = model;
    cached_system_.cpu_cores = physical_ids > 0 ? physical_ids : static_cast<int>(num_cpus_);
    cached_system_.cpu_threads = static_cast<int>(num_cpus_);

    // GPU detection via nvidia-smi
    FILE* fp = popen("nvidia-smi --query-gpu=name,memory.total --format=csv,noheader,nounits 2>/dev/null", "r");
    if (fp) {
        char buf[256];
        if (fgets(buf, sizeof(buf), fp)) {
            std::string result(buf);
            // Format: "GPU Name, 8192"
            auto comma = result.find(',');
            if (comma != std::string::npos) {
                cached_system_.gpu_name = result.substr(0, comma);
                // Trim whitespace
                while (!cached_system_.gpu_name.empty() && cached_system_.gpu_name.back() == ' ')
                    cached_system_.gpu_name.pop_back();
                try {
                    cached_system_.gpu_mem_total_mb = std::stoul(result.substr(comma + 1));
                } catch (...) {}
                cached_system_.has_gpu = true;
            }
        }
        pclose(fp);
    }
}

void SystemMonitor::refresh() {
    auto now = std::chrono::steady_clock::now();

    bool do_proc = (now - last_proc_refresh_ >= kProcInterval);
    bool do_gpu = cached_system_.has_gpu && (now - last_gpu_refresh_ >= kGpuInterval);

    if (do_proc) {
        last_proc_refresh_ = now;
        refreshSystemCpu();   // Must come first — single stat read, computes deltas
        refreshProcesses();   // Uses delta_total from refreshSystemCpu()
        refreshSystemMem();
    }

    if (do_gpu) {
        last_gpu_refresh_ = now;
        refreshGpu();
    }
}

void SystemMonitor::refreshProcesses() {
    if (!pids_handle_) return;

    auto handle = reinterpret_cast<struct pids_info*>(pids_handle_);
    struct pids_fetch* fetched = procps_pids_reap(handle, PIDS_FETCH_TASKS_ONLY);
    if (!fetched) return;

    // Use the delta_total computed by refreshSystemCpu() (called first)
    // This avoids a second procps_stat_get() call which would corrupt the deltas.
    unsigned long long delta_total;
    {
        std::lock_guard lock(mutex_);
        delta_total = delta_total_ticks_;
    }

    std::lock_guard lock(mutex_);

    std::unordered_map<pid_t, ProcessStats> new_procs;
    std::unordered_map<pid_t, unsigned long long> new_ticks;

    for (int i = 0; i < fetched->counts->total; ++i) {
        struct pids_stack* stack = fetched->stacks[i];

        ProcessStats ps;
        ps.pid = PIDS_VAL(PI_PID, s_int, stack, handle);
        ps.ppid = PIDS_VAL(PI_PPID, s_int, stack, handle);
        const char* cmd = PIDS_VAL(PI_CMD, str, stack, handle);
        ps.comm = cmd ? cmd : "";
        unsigned long long tics = PIDS_VAL(PI_TICS_ALL, ull_int, stack, handle);
        ps.mem_rss_kb = PIDS_VAL(PI_VM_RSS, ul_int, stack, handle);
        ps.state = PIDS_VAL(PI_STATE, s_ch, stack, handle);

        // CPU% = delta_proc_ticks / delta_total_ticks * 100
        auto prev_it = prev_proc_ticks_.find(ps.pid);
        if (prev_it != prev_proc_ticks_.end() && delta_total > 0) {
            unsigned long long delta_proc = tics - prev_it->second;
            ps.cpu_percent = static_cast<double>(delta_proc) / static_cast<double>(delta_total) * 100.0 * num_cpus_;
            if (ps.cpu_percent > 100.0 * num_cpus_) ps.cpu_percent = 100.0 * num_cpus_;
            if (ps.cpu_percent < 0.0) ps.cpu_percent = 0.0;
        }

        // Attach GPU memory if available
        auto gpu_it = gpu_proc_mem_.find(ps.pid);
        if (gpu_it != gpu_proc_mem_.end()) {
            ps.gpu_mem_mb = gpu_it->second;
        }

        new_ticks[ps.pid] = tics;
        new_procs[ps.pid] = std::move(ps);
    }

    cached_procs_ = std::move(new_procs);
    prev_proc_ticks_ = std::move(new_ticks);
}

void SystemMonitor::refreshSystemCpu() {
    if (!stat_handle_) return;

    auto sh = reinterpret_cast<struct stat_info*>(stat_handle_);

    // Use select() to read multiple items at once — get() returns the same
    // value for every enum, so it's broken for multi-item queries.
    enum stat_item items[] = {
        STAT_TIC_SUM_TOTAL,    // [0] ull_int
        STAT_TIC_SUM_BUSY,     // [1] ull_int
    };
    auto stack = procps_stat_select(sh, items, 2);
    if (!stack) return;

    unsigned long long total_ticks = stack->head[0].result.ull_int;
    unsigned long long busy_ticks  = stack->head[1].result.ull_int;

    unsigned long long delta_total = total_ticks - prev_total_ticks_;
    unsigned long long delta_busy = busy_ticks - prev_busy_ticks_;

    std::lock_guard lock(mutex_);

    // Store delta_total for per-process CPU% calculation in refreshProcesses()
    delta_total_ticks_ = delta_total;

    if (prev_total_ticks_ > 0 && delta_total > 0) {
        cached_system_.cpu_usage_percent =
            static_cast<double>(delta_busy) / static_cast<double>(delta_total) * 100.0;
        cached_system_.cpu_usage_percent = std::clamp(cached_system_.cpu_usage_percent, 0.0, 100.0);
    }

    prev_total_ticks_ = total_ticks;
    prev_busy_ticks_ = busy_ticks;
}

void SystemMonitor::refreshSystemMem() {
    if (!mem_handle_) return;

    auto mh = reinterpret_cast<struct meminfo_info*>(mem_handle_);

    // Use select() — get() returns the same value for every enum.
    enum meminfo_item items[] = {
        MEMINFO_MEM_TOTAL,      // [0] ul_int
        MEMINFO_MEM_USED,       // [1] ul_int
        MEMINFO_MEM_AVAILABLE,  // [2] ul_int
    };
    auto stack = procps_meminfo_select(mh, items, 3);
    if (!stack) return;

    std::lock_guard lock(mutex_);
    cached_system_.mem_total_kb     = stack->head[0].result.ul_int;
    cached_system_.mem_used_kb      = stack->head[1].result.ul_int;
    cached_system_.mem_available_kb = stack->head[2].result.ul_int;
}

void SystemMonitor::refreshGpu() {
    // System GPU stats
    FILE* fp = popen("nvidia-smi --query-gpu=memory.used,utilization.gpu,temperature.gpu "
                     "--format=csv,noheader,nounits 2>/dev/null", "r");
    if (fp) {
        char buf[256];
        if (fgets(buf, sizeof(buf), fp)) {
            unsigned long mem_used = 0;
            double util = 0, temp = 0;
            if (sscanf(buf, "%lu, %lf, %lf", &mem_used, &util, &temp) >= 1) {
                std::lock_guard lock(mutex_);
                cached_system_.gpu_mem_used_mb = mem_used;
                cached_system_.gpu_utilization = util;
                cached_system_.gpu_temp_c = temp;
            }
        }
        pclose(fp);
    }

    // Per-process GPU memory
    fp = popen("nvidia-smi --query-compute-apps=pid,used_gpu_memory "
               "--format=csv,noheader,nounits 2>/dev/null", "r");
    if (fp) {
        std::unordered_map<pid_t, unsigned long> new_gpu;
        char buf[256];
        while (fgets(buf, sizeof(buf), fp)) {
            pid_t pid = 0;
            unsigned long mem = 0;
            if (sscanf(buf, "%d, %lu", &pid, &mem) == 2 && pid > 0) {
                new_gpu[pid] = mem;
            }
        }
        pclose(fp);

        std::lock_guard lock(mutex_);
        gpu_proc_mem_ = std::move(new_gpu);
    }
}

SystemInfo SystemMonitor::systemInfo() const {
    std::lock_guard lock(mutex_);
    return cached_system_;
}

ProcessStats SystemMonitor::processStats(pid_t pid) const {
    std::lock_guard lock(mutex_);
    auto it = cached_procs_.find(pid);
    if (it == cached_procs_.end()) return {};
    return it->second;
}

ProcessTreeNode SystemMonitor::processTree(pid_t root_pid) const {
    std::lock_guard lock(mutex_);
    return buildTree(root_pid);
}

ProcessTreeNode SystemMonitor::buildTree(pid_t pid) const {
    ProcessTreeNode node;

    auto it = cached_procs_.find(pid);
    if (it != cached_procs_.end()) {
        node.stats = it->second;
    } else {
        node.stats.pid = pid;
    }

    node.total_cpu_percent = node.stats.cpu_percent;
    node.total_mem_rss_kb = node.stats.mem_rss_kb;
    node.total_gpu_mem_mb = node.stats.gpu_mem_mb;

    // Find children
    for (const auto& [child_pid, child_stats] : cached_procs_) {
        if (child_stats.ppid == pid && child_pid != pid) {
            auto child_node = buildTree(child_pid);
            node.total_cpu_percent += child_node.total_cpu_percent;
            node.total_mem_rss_kb += child_node.total_mem_rss_kb;
            node.total_gpu_mem_mb += child_node.total_gpu_mem_mb;
            node.children.push_back(std::move(child_node));
        }
    }

    // Sort children by PID for stable display
    std::sort(node.children.begin(), node.children.end(),
        [](const ProcessTreeNode& a, const ProcessTreeNode& b) {
            return a.stats.pid < b.stats.pid;
        });

    return node;
}

}  // namespace rtl
