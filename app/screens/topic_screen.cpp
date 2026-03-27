#include "screens/topic_screen.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>

using namespace ftxui;

namespace rtl::tui {

TopicScreen::TopicScreen(TopicMonitor* topic_mon)
    : topic_mon_(topic_mon) {}

ftxui::Component TopicScreen::component() {
    auto renderer = Renderer([this] {
        std::lock_guard lock(mutex_);

        // Mode indicator
        auto mode_bar = hbox({
            text(" View: ") | bold,
            text(show_all_ ? "[All Topics]" : "[Watched Only]")
                | color(show_all_ ? Color::Cyan : Color::Yellow),
            filler(),
            text(" " + std::to_string(cached_topics_.size()) + " total topics ") | dim,
        });

        // Header
        auto header = hbox({
            text("   TOPIC") | bold | size(WIDTH, EQUAL, 40),
            text("TYPE") | bold | size(WIDTH, EQUAL, 30),
            text("Hz") | bold | size(WIDTH, EQUAL, 10),
            text("EXPECTED") | bold | size(WIDTH, EQUAL, 10),
            text("PUB") | bold | size(WIDTH, EQUAL, 6),
            text("SUB") | bold | size(WIDTH, EQUAL, 6),
            text("STATUS") | bold | size(WIDTH, EQUAL, 10),
        });

        // Filter topics
        auto topics = cached_topics_;
        if (!show_all_) {
            topics.erase(
                std::remove_if(topics.begin(), topics.end(),
                    [](const TopicInfo& t) {
                        return t.expected_hz <= 0 && t.hz <= 0;
                    }),
                topics.end());
        }

        // Scrollable viewport
        auto term_size = Terminal::Size();
        int max_visible = std::max(3, term_size.dimy - 12);
        int total = (int)topics.size();
        int start = std::clamp(scroll_offset_, 0, std::max(0, total - max_visible));
        int end = std::min(start + max_visible, total);

        Elements rows;
        for (int idx = start; idx < end; ++idx) {
            const auto& t = topics[idx];
            std::ostringstream hz_ss;
            hz_ss << std::fixed << std::setprecision(1) << t.hz;

            std::ostringstream exp_ss;
            if (t.expected_hz > 0) {
                exp_ss << std::fixed << std::setprecision(1) << t.expected_hz;
            } else {
                exp_ss << "-";
            }

            std::string status;
            Color status_color = Color::Green;
            if (t.stale) {
                status = "STALE";
                status_color = Color::Red;
            } else if (t.hz > 0) {
                status = "OK";
                status_color = Color::Green;
            } else if (t.publisher_count > 0) {
                status = "IDLE";
                status_color = Color::Yellow;
            } else {
                status = "NO PUB";
                status_color = Color::GrayDark;
            }

            std::string type_display = t.type;
            if (type_display.size() > 28) {
                type_display = type_display.substr(0, 25) + "...";
            }

            bool is_selected = (idx == selected_);
            std::string prefix = is_selected ? " > " : "   ";

            auto row = hbox({
                text(prefix + t.name) | size(WIDTH, EQUAL, 40),
                text(type_display) | dim | size(WIDTH, EQUAL, 30),
                text(hz_ss.str()) | color(t.stale ? Color::Red : Color::White) | size(WIDTH, EQUAL, 10),
                text(exp_ss.str()) | dim | size(WIDTH, EQUAL, 10),
                text(std::to_string(t.publisher_count)) | size(WIDTH, EQUAL, 6),
                text(std::to_string(t.subscriber_count)) | size(WIDTH, EQUAL, 6),
                text(status) | color(status_color) | size(WIDTH, EQUAL, 10),
            });

            if (is_selected) {
                row = row | inverted;
            }

            rows.push_back(row);
        }

        if (topics.empty()) {
            rows.push_back(text(" No topics to display") | dim);
            if (!show_all_) {
                rows.push_back(text(" Press [a] to show all topics") | dim);
            }
        }

        // Scroll indicator
        std::string scroll_info;
        if (total > max_visible) {
            scroll_info = " [" + std::to_string(start + 1) + "-" + std::to_string(end)
                        + "/" + std::to_string(total) + "]";
        }

        return vbox({
            mode_bar,
            separator(),
            header,
            separator(),
            vbox(std::move(rows)) | flex,
            separator(),
            hbox({
                text(" [a] All topics  [w] Watched only  [Up/Down] Select  [PgUp/PgDn] Scroll" + scroll_info) | dim,
            }),
        });
    });

    return CatchEvent(renderer, [this](Event event) {
        if (event.is_character() && event.character() == "a") {
            std::lock_guard lock(mutex_);
            show_all_ = true;
            return true;
        }
        if (event.is_character() && event.character() == "w") {
            std::lock_guard lock(mutex_);
            show_all_ = false;
            return true;
        }
        if (event == Event::ArrowUp || event == Event::Character("k")) {
            std::lock_guard lock(mutex_);
            if (selected_ > 0) {
                selected_--;
                if (selected_ < scroll_offset_) scroll_offset_ = selected_;
            }
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character("j")) {
            std::lock_guard lock(mutex_);
            int count = (int)cached_topics_.size();
            if (selected_ < count - 1) {
                selected_++;
                auto term_size = Terminal::Size();
                int max_visible = std::max(3, term_size.dimy - 12);
                if (selected_ >= scroll_offset_ + max_visible) {
                    scroll_offset_ = selected_ - max_visible + 1;
                }
            }
            return true;
        }
        if (event == Event::PageUp) {
            std::lock_guard lock(mutex_);
            auto term_size = Terminal::Size();
            int max_visible = std::max(3, term_size.dimy - 12);
            scroll_offset_ = std::max(0, scroll_offset_ - max_visible);
            selected_ = scroll_offset_;
            return true;
        }
        if (event == Event::PageDown) {
            std::lock_guard lock(mutex_);
            auto term_size = Terminal::Size();
            int max_visible = std::max(3, term_size.dimy - 12);
            int count = (int)cached_topics_.size();
            scroll_offset_ = std::min(scroll_offset_ + max_visible, std::max(0, count - max_visible));
            selected_ = scroll_offset_;
            return true;
        }
        return false;
    });
}

void TopicScreen::tick() {
    auto topics = topic_mon_->snapshot();
    std::lock_guard lock(mutex_);
    cached_topics_ = std::move(topics);
}

}  // namespace rtl::tui
