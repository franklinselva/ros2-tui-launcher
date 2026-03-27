#include "screens/log_screen.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>

using namespace ftxui;

namespace rtl::tui {

namespace {
Color levelColor(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:   return Color::GrayDark;
        case LogLevel::Info:    return Color::White;
        case LogLevel::Warn:    return Color::Yellow;
        case LogLevel::Error:   return Color::Red;
        case LogLevel::Fatal:   return Color::RedLight;
        default:                return Color::White;
    }
}

const std::vector<std::string> kLevelNames = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
const std::vector<LogLevel> kLevels = {
    LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Fatal};
}  // namespace

LogScreen::LogScreen(LogAggregator* log_agg, NodeInspector* node_inspector)
    : log_agg_(log_agg), node_inspector_(node_inspector) {}

ftxui::Component LogScreen::component() {
    // Set static level options once
    level_filter_.setOptions(kLevelNames);

    auto renderer = Renderer([this] {
        std::lock_guard lock(mutex_);

        // Adapt viewport to terminal height
        auto term_size = Terminal::Size();
        int viewport_h = std::max(5, term_size.dimy - 10);
        scroll_list_.setViewportHeight(viewport_h);
        scroll_list_.setItemCount((int)cached_entries_.size());

        auto filter_bar = hbox({
            source_filter_.renderInline(),
            level_filter_.renderInline(),
            node_filter_.renderInline(),
            search_bar_.render(),
            filler(),
            text(" " + std::to_string(cached_entries_.size()) + " lines ") | dim,
            text(scroll_list_.autoScroll() ? " AUTO " : " MANUAL ")
                | (scroll_list_.autoScroll() ? color(Color::Green) : color(Color::Yellow)),
        });

        // Log entries
        auto [start, end] = scroll_list_.visibleRange();
        Elements log_lines;
        for (int i = start; i < end; ++i) {
            const auto& entry = cached_entries_[i];
            auto lvl_color = levelColor(entry.level);
            auto ts = formatTimestamp(entry.wall_time);

            log_lines.push_back(
                hbox({
                    text(" " + ts) | dim | size(WIDTH, EQUAL, 14),
                    text(std::string(logLevelStr(entry.level)))
                        | color(lvl_color) | size(WIDTH, EQUAL, 6),
                    text(entry.source) | color(Color::Cyan) | size(WIDTH, EQUAL, 22),
                    text(" " + entry.message),
                }));
        }

        if (log_lines.empty()) {
            log_lines.push_back(text(" No log entries") | dim);
        }

        // Scroll indicator
        int total = (int)cached_entries_.size();
        std::string scroll_info;
        if (total > viewport_h) {
            int page = start / std::max(1, viewport_h);
            int total_pages = (total + viewport_h - 1) / viewport_h;
            scroll_info = " [" + std::to_string(page + 1) + "/" + std::to_string(total_pages) + "]";
        }

        auto content = vbox({
            filter_bar,
            separator(),
            vbox(std::move(log_lines)) | flex,
            separator(),
            hbox({
                text(" [f] Source  [v] Level  [n] Node  [/] Search  [c] Clear  [Space] Auto-scroll  [Up/Down] Scroll" + scroll_info) | dim,
            }),
        });

        // Overlay any open dropdown
        auto dropdown = source_filter_.renderDropdown();
        if (source_filter_.inputActive()) {
            return dbox({content, dropdown | center});
        }
        dropdown = level_filter_.renderDropdown();
        if (level_filter_.inputActive()) {
            return dbox({content, dropdown | center});
        }
        dropdown = node_filter_.renderDropdown();
        if (node_filter_.inputActive()) {
            return dbox({content, dropdown | center});
        }

        return content;
    });

    return CatchEvent(renderer, [this](Event event) {
        std::lock_guard lock(mutex_);

        // Dropdowns get first crack (when open they consume all events)
        if (source_filter_.handleEvent(event)) return true;
        if (level_filter_.handleEvent(event)) return true;
        if (node_filter_.handleEvent(event)) return true;

        // Search bar
        if (search_bar_.handleEvent(event)) return true;

        // Scroll list handles navigation
        if (scroll_list_.handleEvent(event)) return true;

        // Screen-specific keys
        if (event.is_character() && event.character() == "c") {
            log_agg_->clear();
            return true;
        }
        if (event.is_character() && event.character() == " ") {
            scroll_list_.toggleAutoScroll();
            return true;
        }
        return false;
    });
}

void LogScreen::tick() {
    // Read filter state
    std::string source_filter;
    LogLevel min_level = LogLevel::Debug;
    std::string search;

    {
        std::lock_guard lock(mutex_);

        // Source filter from dropdown
        source_filter = source_filter_.selectedValue();

        // Level filter from dropdown
        int level_idx = level_filter_.selected();
        if (level_idx > 0 && level_idx <= (int)kLevels.size()) {
            min_level = kLevels[level_idx - 1];
        }

        search = search_bar_.query();

        // Node filter overrides source filter
        std::string node_val = node_filter_.selectedValue();
        if (!node_val.empty()) {
            source_filter = node_val;
        }
    }

    auto entries = log_agg_->filtered(source_filter, min_level, search);
    auto sources = log_agg_->sources();

    // Ensure node data is fresh (refresh is internally throttled to 2s)
    node_inspector_->refresh();
    auto nodes = node_inspector_->nodes();
    std::vector<std::string> node_names;
    node_names.reserve(nodes.size());
    for (const auto& n : nodes) {
        // Log sources use node name without leading "/" (from /rosout msg.name)
        std::string display = n.full_name;
        if (!display.empty() && display[0] == '/') {
            display = display.substr(1);
        }
        node_names.push_back(display);
    }
    std::sort(node_names.begin(), node_names.end());

    std::lock_guard lock(mutex_);
    cached_entries_ = std::move(entries);
    source_filter_.setOptions(sources);
    node_filter_.setOptions(node_names);
}

}  // namespace rtl::tui
