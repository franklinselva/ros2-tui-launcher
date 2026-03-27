#include "screens/launch_screen.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

#include <algorithm>
#include <thread>

using namespace ftxui;

namespace rtl::tui {

LaunchScreen::LaunchScreen(
    std::vector<LaunchProfile>* profiles,
    int* active_profile_idx,
    ProcessManager* proc_mgr)
    : profiles_(profiles)
    , active_profile_idx_(active_profile_idx)
    , proc_mgr_(proc_mgr) {}

void LaunchScreen::startSelected() {
    if (profiles_->empty()) return;
    const auto& profile = (*profiles_)[*active_profile_idx_];
    if (selected_entry_ < 0 || selected_entry_ >= (int)profile.entries.size()) return;
    // Run in background thread to avoid blocking UI
    std::thread([this, entry = profile.entries[selected_entry_]]() {
        proc_mgr_->start(entry);
    }).detach();
}

void LaunchScreen::stopSelected() {
    if (profiles_->empty()) return;
    const auto& profile = (*profiles_)[*active_profile_idx_];
    if (selected_entry_ < 0 || selected_entry_ >= (int)profile.entries.size()) return;
    // Run in background thread — stop() can block up to 5s
    std::thread([this, name = profile.entries[selected_entry_].displayName()]() {
        proc_mgr_->stop(name);
    }).detach();
}

void LaunchScreen::restartSelected() {
    if (profiles_->empty()) return;
    const auto& profile = (*profiles_)[*active_profile_idx_];
    if (selected_entry_ < 0 || selected_entry_ >= (int)profile.entries.size()) return;
    std::thread([this, name = profile.entries[selected_entry_].displayName()]() {
        proc_mgr_->restart(name);
    }).detach();
}

void LaunchScreen::startAll() {
    if (profiles_->empty()) return;
    const auto& profile = (*profiles_)[*active_profile_idx_];
    std::thread([this, entries = profile.entries]() {
        for (const auto& entry : entries) {
            proc_mgr_->start(entry);
        }
    }).detach();
}

void LaunchScreen::stopAll() {
    std::thread([this]() {
        proc_mgr_->stopAll();
    }).detach();
}

void LaunchScreen::nextProfile() {
    if (profiles_->size() <= 1) return;
    *active_profile_idx_ = (*active_profile_idx_ + 1) % (int)profiles_->size();
    selected_entry_ = 0;
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

        Elements header_row;
        header_row.push_back(
            hbox({
                text(" Profile: ") | bold,
                text(profile.name) | color(Color::Cyan),
                text("  (" + std::to_string(profile.entries.size()) + " entries)") | dim,
            }));
        if (!profile.description.empty()) {
            header_row.push_back(text("  " + profile.description) | dim);
        }

        // Profile selector
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

        // Process table
        Elements table_rows;
        table_rows.push_back(separator());
        table_rows.push_back(
            hbox({
                text("   NAME") | bold | size(WIDTH, EQUAL, 32),
                text("STATE") | bold | size(WIDTH, EQUAL, 12),
                text("PID") | bold | size(WIDTH, EQUAL, 10),
                text("UPTIME") | bold | size(WIDTH, EQUAL, 15),
                text("RESTART") | bold | size(WIDTH, EQUAL, 12),
            }));
        table_rows.push_back(separator());

        int entry_idx = 0;
        for (const auto& entry : profile.entries) {
            std::string proc_name = entry.displayName();

            // Find matching process info
            ProcessInfo pinfo;
            pinfo.name = proc_name;
            for (const auto& p : cached_procs_) {
                if (p.name == proc_name) {
                    pinfo = p;
                    break;
                }
            }

            // State color
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

            // Selection indicator
            bool is_selected = (entry_idx == selected_entry_);
            std::string prefix = is_selected ? " > " : "   ";

            auto row = hbox({
                text(prefix + proc_name) | size(WIDTH, EQUAL, 32)
                    | (is_selected ? bold : nothing),
                text(processStateStr(state)) | color(state_color) | size(WIDTH, EQUAL, 12),
                text(pid_str) | size(WIDTH, EQUAL, 10),
                text(uptime) | size(WIDTH, EQUAL, 15),
                text(entry.restart_policy) | dim | size(WIDTH, EQUAL, 12),
            });

            if (is_selected) {
                row = row | inverted;
            }

            table_rows.push_back(row);
            entry_idx++;
        }

        Elements result;
        for (auto& r : header_row) result.push_back(r);
        for (auto& r : table_rows) result.push_back(r);

        result.push_back(filler());
        result.push_back(separator());
        result.push_back(
            hbox({
                text(" [Up/Down] Select  [Enter] Toggle  [r] Restart  [a] Start All  [s] Stop All  [p] Profile ") | dim,
            }));

        return vbox(std::move(result));
    });

    // Wrap with event handler
    return CatchEvent(renderer, [this](Event event) {
        if (profiles_->empty()) return false;
        const auto& profile = (*profiles_)[*active_profile_idx_];
        int entry_count = (int)profile.entries.size();

        if (event == Event::ArrowUp || event == Event::Character("k")) {
            std::lock_guard lock(mutex_);
            if (selected_entry_ > 0) selected_entry_--;
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character("j")) {
            std::lock_guard lock(mutex_);
            if (selected_entry_ < entry_count - 1) selected_entry_++;
            return true;
        }
        if (event == Event::Return) {
            // Toggle: start if stopped, stop if running
            std::string proc_name = profile.entries[selected_entry_].displayName();
            auto info = proc_mgr_->processInfo(proc_name);
            if (info.state.load() == ProcessState::Running || info.state.load() == ProcessState::Starting) {
                stopSelected();
            } else {
                startSelected();
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
            std::lock_guard lock(mutex_);
            nextProfile();
            return true;
        }
        return false;
    });
}

void LaunchScreen::tick() {
    std::lock_guard lock(mutex_);
    cached_procs_ = proc_mgr_->processes();
}

}  // namespace rtl::tui
