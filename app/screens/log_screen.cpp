#include "screens/log_screen.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
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

LogScreen::LogScreen(LogAggregator* log_agg)
    : log_agg_(log_agg) {}

ftxui::Component LogScreen::component() {
    auto renderer = Renderer([this] {
        std::lock_guard lock(mutex_);

        // Adapt viewport to terminal height (subtract chrome: header, filter bar, separators, hotkey bar, border)
        auto term_size = Terminal::Size();
        viewport_height_ = std::max(5, term_size.dimy - 10);

        // Filter bar
        std::string source_label = (selected_source_ == 0)
            ? "All"
            : (selected_source_ <= (int)cached_sources_.size()
                ? cached_sources_[selected_source_ - 1] : "All");
        std::string level_label = (selected_level_ < (int)kLevelNames.size())
            ? kLevelNames[selected_level_] : "ALL";

        auto filter_bar = hbox({
            text(" Source: ") | bold,
            text("[" + source_label + "]") | color(Color::Cyan),
            text("  Level: ") | bold,
            text("[" + level_label + "+]") | color(Color::Yellow),
            text("  Search: ") | bold,
            text(search_mode_ ? search_text_ + "_" : (search_text_.empty() ? "(none)" : search_text_))
                | (search_mode_ ? color(Color::White) : dim),
            search_mode_ ? (text(" [ESC] cancel ") | color(Color::Yellow)) : text(""),
            filler(),
            text(" " + std::to_string(cached_entries_.size()) + " lines ") | dim,
            text(auto_scroll_ ? " AUTO " : " MANUAL ") | (auto_scroll_ ? color(Color::Green) : color(Color::Yellow)),
        });

        // Log entries
        Elements log_lines;
        int max_visible = viewport_height_;
        int total = (int)cached_entries_.size();
        int start = auto_scroll_
            ? std::max(0, total - max_visible)
            : std::clamp(scroll_offset_, 0, std::max(0, total - max_visible));
        int end = std::min(start + max_visible, total);

        for (int i = start; i < end; ++i) {
            const auto& entry = cached_entries_[i];
            auto lvl_color = levelColor(entry.level);
            auto ts = formatTimestamp(entry.wall_time);

            log_lines.push_back(
                hbox({
                    text(" " + ts)
                        | dim
                        | size(WIDTH, EQUAL, 14),
                    text(std::string(logLevelStr(entry.level)))
                        | color(lvl_color)
                        | size(WIDTH, EQUAL, 6),
                    text(entry.source)
                        | color(Color::Cyan)
                        | size(WIDTH, EQUAL, 22),
                    text(" " + entry.message),
                }));
        }

        if (log_lines.empty()) {
            log_lines.push_back(text(" No log entries") | dim);
        }

        // Scroll indicator
        std::string scroll_info;
        if (total > max_visible) {
            int page = start / std::max(1, max_visible);
            int total_pages = (total + max_visible - 1) / max_visible;
            scroll_info = " [" + std::to_string(page + 1) + "/" + std::to_string(total_pages) + "]";
        }

        return vbox({
            filter_bar,
            separator(),
            vbox(std::move(log_lines)) | flex,
            separator(),
            hbox({
                text(" [f] Source  [v] Level  [/] Search  [c] Clear  [Space] Auto-scroll  [Up/Down] Scroll" + scroll_info) | dim,
            }),
        });
    });

    return CatchEvent(renderer, [this](Event event) {
        // Search mode: capture typed characters
        if (search_mode_) {
            if (event == Event::Escape || event == Event::Return) {
                search_mode_ = false;
                return true;
            }
            if (event == Event::Backspace) {
                std::lock_guard lock(mutex_);
                if (!search_text_.empty()) search_text_.pop_back();
                return true;
            }
            if (event.is_character()) {
                std::lock_guard lock(mutex_);
                search_text_ += event.character();
                return true;
            }
            return false;
        }

        // Normal mode
        if (event.is_character() && event.character() == "/") {
            std::lock_guard lock(mutex_);
            search_mode_ = true;
            search_text_.clear();
            return true;
        }
        if (event.is_character() && event.character() == "f") {
            std::lock_guard lock(mutex_);
            int source_count = (int)cached_sources_.size();
            selected_source_ = (selected_source_ + 1) % (source_count + 1);
            return true;
        }
        if (event.is_character() && event.character() == "v") {
            std::lock_guard lock(mutex_);
            selected_level_ = (selected_level_ + 1) % (int)kLevelNames.size();
            return true;
        }
        if (event.is_character() && event.character() == "c") {
            log_agg_->clear();
            return true;
        }
        if (event.is_character() && event.character() == " ") {
            std::lock_guard lock(mutex_);
            auto_scroll_ = !auto_scroll_;
            return true;
        }
        if (event == Event::ArrowUp || event == Event::Character("k")) {
            std::lock_guard lock(mutex_);
            auto_scroll_ = false;
            if (scroll_offset_ > 0) scroll_offset_--;
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character("j")) {
            std::lock_guard lock(mutex_);
            auto_scroll_ = false;
            int max_offset = std::max(0, (int)cached_entries_.size() - viewport_height_);
            if (scroll_offset_ < max_offset) scroll_offset_++;
            return true;
        }
        if (event == Event::PageUp) {
            std::lock_guard lock(mutex_);
            auto_scroll_ = false;
            scroll_offset_ = std::max(0, scroll_offset_ - viewport_height_);
            return true;
        }
        if (event == Event::PageDown) {
            std::lock_guard lock(mutex_);
            auto_scroll_ = false;
            int max_offset = std::max(0, (int)cached_entries_.size() - viewport_height_);
            scroll_offset_ = std::min(scroll_offset_ + viewport_height_, max_offset);
            return true;
        }
        if (event == Event::Home) {
            std::lock_guard lock(mutex_);
            auto_scroll_ = false;
            scroll_offset_ = 0;
            return true;
        }
        if (event == Event::End) {
            std::lock_guard lock(mutex_);
            auto_scroll_ = true;
            return true;
        }

        // Mouse wheel scroll
        if (event.is_mouse()) {
            auto& mouse = event.mouse();
            if (mouse.button == Mouse::WheelDown) {
                std::lock_guard lock(mutex_);
                auto_scroll_ = false;
                int max_offset = std::max(0, (int)cached_entries_.size() - viewport_height_);
                scroll_offset_ = std::min(scroll_offset_ + 3, max_offset);
                return true;
            }
            if (mouse.button == Mouse::WheelUp) {
                std::lock_guard lock(mutex_);
                auto_scroll_ = false;
                scroll_offset_ = std::max(0, scroll_offset_ - 3);
                return true;
            }
        }

        return false;
    });
}

void LogScreen::tick() {
    std::string source_filter;
    LogLevel min_level = LogLevel::Debug;
    std::string search;

    {
        std::lock_guard lock(mutex_);
        if (selected_source_ > 0 && selected_source_ <= (int)cached_sources_.size()) {
            source_filter = cached_sources_[selected_source_ - 1];
        }
        if (selected_level_ < (int)kLevels.size()) {
            min_level = kLevels[selected_level_];
        }
        search = search_text_;  // copy under lock to fix data race
    }

    auto entries = log_agg_->filtered(source_filter, min_level, search);
    auto sources = log_agg_->sources();

    std::lock_guard lock(mutex_);
    cached_entries_ = std::move(entries);
    cached_sources_ = std::move(sources);
}

}  // namespace rtl::tui
