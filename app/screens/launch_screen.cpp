#include "screens/launch_screen.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

#include <algorithm>
#include <functional>
#include <sstream>

using namespace ftxui;

namespace rtl::tui {

LaunchScreen::LaunchScreen(
    std::vector<LaunchProfile>* profiles,
    int* active_profile_idx,
    ProcessManager* proc_mgr,
    SystemMonitor* sys_mon)
    : profiles_(profiles)
    , active_profile_idx_(active_profile_idx)
    , proc_mgr_(proc_mgr)
    , sys_mon_(sys_mon) {}

LaunchScreen::~LaunchScreen() {
    // Wait for all background operations to complete
    std::lock_guard lock(bg_mutex_);
    for (auto& f : bg_futures_) {
        if (f.valid()) f.wait();
    }
}

void LaunchScreen::launchAsync(std::function<void()> fn) {
    std::lock_guard lock(bg_mutex_);
    cleanupFinishedFutures();
    bg_futures_.push_back(std::async(std::launch::async, std::move(fn)));
}

void LaunchScreen::cleanupFinishedFutures() {
    bg_futures_.erase(
        std::remove_if(bg_futures_.begin(), bg_futures_.end(),
            [](const std::future<void>& f) {
                return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            }),
        bg_futures_.end());
}

void LaunchScreen::startSelected() {
    if (profiles_->empty()) return;
    const auto& profile = (*profiles_)[*active_profile_idx_];
    int sel = scroll_list_.selected();
    if (sel < 0 || sel >= (int)profile.entries.size()) return;
    launchAsync([this, entry = profile.entries[sel]]() {
        proc_mgr_->start(entry);
    });
}

void LaunchScreen::stopSelected() {
    if (profiles_->empty()) return;
    const auto& profile = (*profiles_)[*active_profile_idx_];
    int sel = scroll_list_.selected();
    if (sel < 0 || sel >= (int)profile.entries.size()) return;
    launchAsync([this, name = profile.entries[sel].displayName()]() {
        proc_mgr_->stop(name);
    });
}

void LaunchScreen::restartSelected() {
    if (profiles_->empty()) return;
    const auto& profile = (*profiles_)[*active_profile_idx_];
    int sel = scroll_list_.selected();
    if (sel < 0 || sel >= (int)profile.entries.size()) return;
    launchAsync([this, name = profile.entries[sel].displayName()]() {
        proc_mgr_->restart(name);
    });
}

void LaunchScreen::startAll() {
    if (profiles_->empty()) return;
    const auto& profile = (*profiles_)[*active_profile_idx_];
    launchAsync([this, entries = profile.entries]() {
        for (const auto& entry : entries) {
            proc_mgr_->start(entry);
        }
    });
}

void LaunchScreen::stopAll() {
    launchAsync([this]() {
        proc_mgr_->stopAll();
    });
}

void LaunchScreen::nextProfile() {
    if (profiles_->size() <= 1) return;
    *active_profile_idx_ = (*active_profile_idx_ + 1) % (int)profiles_->size();
}

std::string LaunchScreen::formatMemKb(unsigned long kb) {
    if (kb >= 1048576) {  // >= 1 GB
        std::ostringstream oss;
        oss.precision(1);
        oss << std::fixed << static_cast<double>(kb) / 1048576.0 << " GB";
        return oss.str();
    }
    return std::to_string(kb / 1024) + " MB";
}

std::string LaunchScreen::formatMemMb(unsigned long mb) {
    if (mb >= 1024) {
        std::ostringstream oss;
        oss.precision(1);
        oss << std::fixed << static_cast<double>(mb) / 1024.0 << " GB";
        return oss.str();
    }
    return std::to_string(mb) + " MB";
}

std::string LaunchScreen::fmtPercent(double pct) {
    if (pct < 0.05) return "0%";
    std::ostringstream oss;
    oss.precision(1);
    oss << std::fixed << pct << "%";
    return oss.str();
}

Element LaunchScreen::renderGauges() {
    Elements lines;

    // CPU line
    lines.push_back(hbox({
        text(" " + cached_sys_.cpu_model) | bold,
        text("  " + std::to_string(cached_sys_.cpu_cores) + "C/" +
             std::to_string(cached_sys_.cpu_threads) + "T") | dim,
    }));

    double cpu_frac = std::clamp(cached_sys_.cpu_usage_percent / 100.0, 0.0, 1.0);
    double mem_frac = (cached_sys_.mem_total_kb > 0)
        ? std::clamp(static_cast<double>(cached_sys_.mem_used_kb) /
                     static_cast<double>(cached_sys_.mem_total_kb), 0.0, 1.0)
        : 0.0;

    auto cpu_color = (cpu_frac < 0.5) ? Color::Green : (cpu_frac < 0.8 ? Color::Yellow : Color::Red);
    auto mem_color = (mem_frac < 0.5) ? Color::Green : (mem_frac < 0.8 ? Color::Yellow : Color::Red);

    lines.push_back(hbox({
        text(" CPU ") | bold,
        gauge(cpu_frac) | flex | color(cpu_color),
        text(" " + fmtPercent(cached_sys_.cpu_usage_percent)) | size(WIDTH, EQUAL, 7),
        text("  MEM ") | bold,
        gauge(mem_frac) | flex | color(mem_color),
        text(" " + formatMemKb(cached_sys_.mem_used_kb) + "/" +
             formatMemKb(cached_sys_.mem_total_kb)) | size(WIDTH, EQUAL, 18),
    }));

    // GPU lines (if present)
    if (cached_sys_.has_gpu) {
        double gpu_frac = std::clamp(cached_sys_.gpu_utilization / 100.0, 0.0, 1.0);
        double vram_frac = (cached_sys_.gpu_mem_total_mb > 0)
            ? std::clamp(static_cast<double>(cached_sys_.gpu_mem_used_mb) /
                         static_cast<double>(cached_sys_.gpu_mem_total_mb), 0.0, 1.0)
            : 0.0;

        lines.push_back(hbox({
            text(" " + cached_sys_.gpu_name) | bold,
            text("  " + formatMemMb(cached_sys_.gpu_mem_total_mb)) | dim,
            text("  " + std::to_string(static_cast<int>(cached_sys_.gpu_temp_c)) + "\u00b0C") | dim,
        }));

        auto gpu_color = (gpu_frac < 0.5) ? Color::Green : (gpu_frac < 0.8 ? Color::Yellow : Color::Red);
        auto vram_color = (vram_frac < 0.5) ? Color::Green : (vram_frac < 0.8 ? Color::Yellow : Color::Red);

        lines.push_back(hbox({
            text(" GPU ") | bold,
            gauge(gpu_frac) | flex | color(gpu_color),
            text(" " + fmtPercent(cached_sys_.gpu_utilization)) | size(WIDTH, EQUAL, 7),
            text("  VRAM ") | bold,
            gauge(vram_frac) | flex | color(vram_color),
            text(" " + formatMemMb(cached_sys_.gpu_mem_used_mb) + "/" +
                 formatMemMb(cached_sys_.gpu_mem_total_mb)) | size(WIDTH, EQUAL, 18),
        }));
    }

    return window(text(" System "), vbox(std::move(lines)));
}

ftxui::Component LaunchScreen::component() {
    auto renderer = Renderer([this] {
        std::lock_guard lock(mutex_);

        if (profiles_->empty()) {
            return vbox({
                text(" No profiles loaded") | color(Color::Yellow),
                text(" Place .yaml files in the profiles directory"),
            });
        }

        const auto& profile = (*profiles_)[*active_profile_idx_];

        // Update scroll list item count (entry count, not expanded rows)
        scroll_list_.setItemCount((int)profile.entries.size());

        // --- System gauges ---
        auto gauges = renderGauges();

        // --- Profile header ---
        Elements header_row;
        auto profile_line = hbox({
            text(" Profile: ") | bold,
            text(profile.name) | color(Color::Cyan),
            text("  (" + std::to_string(profile.entries.size()) + " entries)") | dim,
        });
        header_row.push_back(profile_line);

        if (!profile.description.empty()) {
            header_row.push_back(text("  " + profile.description) | dim);
        }

        if (profiles_->size() > 1) {
            Elements prof_items;
            prof_items.push_back(text(" Profiles: ") | dim);
            for (size_t i = 0; i < profiles_->size(); ++i) {
                if ((int)i == *active_profile_idx_) {
                    prof_items.push_back(
                        text("[" + (*profiles_)[i].name + "]") | bold | color(Color::Cyan));
                } else {
                    prof_items.push_back(
                        text(" " + (*profiles_)[i].name + " ") | dim);
                }
            }
            header_row.push_back(hbox(std::move(prof_items)));
        }

        // --- Process table ---
        bool has_gpu = cached_sys_.has_gpu;

        Elements table_rows;
        table_rows.push_back(separator());

        // Table header
        Elements hdr_cols = {
            text("   NAME") | bold | size(WIDTH, EQUAL, 28),
            text("STATE") | bold | size(WIDTH, EQUAL, 10),
            text("PID") | bold | size(WIDTH, EQUAL, 8),
            text("CPU%") | bold | size(WIDTH, EQUAL, 8),
            text("MEM") | bold | size(WIDTH, EQUAL, 10),
        };
        if (has_gpu) {
            hdr_cols.push_back(text("GPU") | bold | size(WIDTH, EQUAL, 10));
        }
        hdr_cols.push_back(text("UPTIME") | bold | size(WIDTH, EQUAL, 10));
        hdr_cols.push_back(text("RESTART") | bold | size(WIDTH, EQUAL, 10));

        table_rows.push_back(hbox(std::move(hdr_cols)));
        table_rows.push_back(separator());

        int selected = scroll_list_.selected();
        int entry_idx = 0;

        for (const auto& entry : profile.entries) {
            std::string proc_name = entry.displayName();

            // Find matching process info via O(1) lookup
            ProcessInfo pinfo;
            pinfo.name = proc_name;
            auto proc_it = cached_procs_.find(proc_name);
            if (proc_it != cached_procs_.end()) {
                pinfo = proc_it->second;
            }

            auto state = pinfo.state.load();
            Color state_color = Color::White;
            switch (state) {
                case ProcessState::Running:  state_color = Color::Green; break;
                case ProcessState::Starting: state_color = Color::Yellow; break;
                case ProcessState::Crashed:  state_color = Color::Red; break;
                case ProcessState::Stopping: state_color = Color::Yellow; break;
                case ProcessState::Stopped:  state_color = Color::GrayDark; break;
            }

            // Uptime
            std::string uptime = "-";
            if (state == ProcessState::Running) {
                auto dur = std::chrono::steady_clock::now() - pinfo.started_at;
                auto secs = std::chrono::duration_cast<std::chrono::seconds>(dur).count();
                if (secs >= 3600) {
                    uptime = std::to_string(secs / 3600) + "h " +
                             std::to_string((secs % 3600) / 60) + "m";
                } else if (secs >= 60) {
                    uptime = std::to_string(secs / 60) + "m " +
                             std::to_string(secs % 60) + "s";
                } else {
                    uptime = std::to_string(secs) + "s";
                }
            }

            std::string pid_str = (pinfo.pid > 0) ? std::to_string(pinfo.pid) : "-";

            // Get tree info
            auto tree_it = cached_trees_.find(proc_name);
            bool has_children = (tree_it != cached_trees_.end() && !tree_it->second.children.empty());
            bool is_expanded = expanded_.count(proc_name) && expanded_[proc_name];

            // Tree prefix
            bool is_selected = (entry_idx == selected);
            std::string tree_icon = "   ";  // no children
            if (has_children) {
                tree_icon = is_expanded ? " \u25bc " : " \u25b6 ";
            }
            std::string prefix = is_selected ? " >" : "  ";

            // CPU/MEM values — show tree totals for root entry
            std::string cpu_str = "-";
            std::string mem_str = "-";
            std::string gpu_str = "-";
            if (state == ProcessState::Running && tree_it != cached_trees_.end()) {
                cpu_str = fmtPercent(tree_it->second.total_cpu_percent);
                mem_str = formatMemKb(tree_it->second.total_mem_rss_kb);
                if (has_gpu && tree_it->second.total_gpu_mem_mb > 0) {
                    gpu_str = formatMemMb(tree_it->second.total_gpu_mem_mb);
                }
            }

            // Build row
            Elements row_cols = {
                text(prefix + tree_icon + proc_name) | size(WIDTH, EQUAL, 28)
                    | (is_selected ? bold : nothing),
                text(processStateStr(state)) | color(state_color) | size(WIDTH, EQUAL, 10),
                text(pid_str) | size(WIDTH, EQUAL, 8),
                text(cpu_str) | size(WIDTH, EQUAL, 8),
                text(mem_str) | size(WIDTH, EQUAL, 10),
            };
            if (has_gpu) {
                row_cols.push_back(text(gpu_str) | size(WIDTH, EQUAL, 10));
            }
            row_cols.push_back(text(uptime) | size(WIDTH, EQUAL, 10));
            row_cols.push_back(text(entry.restart_policy) | dim | size(WIDTH, EQUAL, 10));

            auto row = hbox(std::move(row_cols));
            if (is_selected) {
                row = row | inverted;
            }
            table_rows.push_back(row);

            // Expanded children rows
            if (has_children && is_expanded && tree_it != cached_trees_.end()) {
                const auto& children = tree_it->second.children;
                for (size_t ci = 0; ci < children.size(); ++ci) {
                    const auto& child = children[ci];
                    bool is_last = (ci == children.size() - 1);
                    std::string tree_line = is_last ? "   \u2514\u2500 " : "   \u251c\u2500 ";

                    std::string c_cpu = fmtPercent(child.stats.cpu_percent);
                    std::string c_mem = formatMemKb(child.stats.mem_rss_kb);
                    std::string c_gpu = "-";
                    if (has_gpu && child.stats.gpu_mem_mb > 0) {
                        c_gpu = formatMemMb(child.stats.gpu_mem_mb);
                    }
                    std::string c_state(1, child.stats.state);

                    Elements child_cols = {
                        text(tree_line + child.stats.comm) | dim | size(WIDTH, EQUAL, 28),
                        text(c_state) | dim | size(WIDTH, EQUAL, 10),
                        text(std::to_string(child.stats.pid)) | dim | size(WIDTH, EQUAL, 8),
                        text(c_cpu) | dim | size(WIDTH, EQUAL, 8),
                        text(c_mem) | dim | size(WIDTH, EQUAL, 10),
                    };
                    if (has_gpu) {
                        child_cols.push_back(text(c_gpu) | dim | size(WIDTH, EQUAL, 10));
                    }
                    child_cols.push_back(text("") | size(WIDTH, EQUAL, 10));  // uptime
                    child_cols.push_back(text("") | size(WIDTH, EQUAL, 10));  // restart

                    table_rows.push_back(hbox(std::move(child_cols)));
                }
            }

            entry_idx++;
        }

        // Assemble final layout
        Elements result;
        result.push_back(gauges);
        for (auto& r : header_row) result.push_back(r);
        for (auto& r : table_rows) result.push_back(r);

        result.push_back(filler());
        result.push_back(separator());
        result.push_back(
            hbox({
                text(" [\u2191\u2193] Select  [Enter] Toggle  [e] Expand  [r] Restart  [a] All  [s] Stop All  [p] Profile ") | dim,
            }));

        return vbox(std::move(result));
    });

    return CatchEvent(renderer, [this](Event event) {
        if (profiles_->empty()) return false;

        std::lock_guard lock(mutex_);

        // Scroll list handles navigation
        if (scroll_list_.handleEvent(event)) return true;

        // Toggle start/stop
        if (event == Event::Return) {
            const auto& profile = (*profiles_)[*active_profile_idx_];
            int sel = scroll_list_.selected();
            if (sel >= 0 && sel < (int)profile.entries.size()) {
                std::string proc_name = profile.entries[sel].displayName();
                auto info = proc_mgr_->processInfo(proc_name);
                if (info.state.load() == ProcessState::Running || info.state.load() == ProcessState::Starting) {
                    stopSelected();
                } else {
                    startSelected();
                }
            }
            return true;
        }

        // Expand/collapse tree
        if (event.is_character() && event.character() == "e") {
            const auto& profile = (*profiles_)[*active_profile_idx_];
            int sel = scroll_list_.selected();
            if (sel >= 0 && sel < (int)profile.entries.size()) {
                std::string proc_name = profile.entries[sel].displayName();
                expanded_[proc_name] = !expanded_[proc_name];
            }
            return true;
        }

        if (event.is_character() && event.character() == "r") {
            restartSelected();
            return true;
        }
        if (event.is_character() && event.character() == "a") {
            startAll();
            return true;
        }
        if (event.is_character() && event.character() == "s") {
            stopAll();
            return true;
        }
        if (event.is_character() && event.character() == "p") {
            nextProfile();
            return true;
        }
        return false;
    });
}

void LaunchScreen::tick() {
    sys_mon_->refresh();

    auto sys_info = sys_mon_->systemInfo();
    auto procs_vec = proc_mgr_->processes();

    // Build name→info map for O(1) lookup in render
    std::unordered_map<std::string, ProcessInfo> procs_map;
    procs_map.reserve(procs_vec.size());

    // Build trees for running processes
    std::unordered_map<std::string, ProcessTreeNode> trees;
    for (const auto& p : procs_vec) {
        if (p.state.load() == ProcessState::Running && p.pid > 0) {
            trees[p.name] = sys_mon_->processTree(p.pid);
        }
        procs_map.emplace(p.name, p);
    }

    std::lock_guard lock(mutex_);
    cached_procs_ = std::move(procs_map);
    cached_sys_ = std::move(sys_info);
    cached_trees_ = std::move(trees);
}

}  // namespace rtl::tui
