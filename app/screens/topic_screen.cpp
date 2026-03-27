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

TopicScreen::TopicScreen(TopicMonitor* topic_mon, NodeInspector* node_inspector)
    : topic_mon_(topic_mon), node_inspector_(node_inspector) {}

ftxui::Component TopicScreen::component() {
    auto renderer = Renderer([this] {
        std::lock_guard lock(mutex_);

        // Adapt viewport
        auto term_size = Terminal::Size();
        int viewport_h = std::max(3, term_size.dimy - 12);

        // Mode indicator + node filter + search
        auto mode_bar = hbox({
            text(" View: ") | bold,
            text(show_all_ ? "[All Topics]" : "[Watched Only]")
                | color(show_all_ ? Color::Cyan : Color::Yellow),
            node_filter_.renderInline(),
            search_bar_.render(),
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

        // Search filter
        auto query = search_bar_.query();
        if (!query.empty()) {
            topics.erase(
                std::remove_if(topics.begin(), topics.end(),
                    [&query](const TopicInfo& t) {
                        return t.name.find(query) == std::string::npos &&
                               t.type.find(query) == std::string::npos;
                    }),
                topics.end());
        }

        // Node filter
        std::string node_val = node_filter_.selectedValue();
        if (!node_val.empty()) {
            auto it = cached_node_topics_.find(node_val);
            if (it != cached_node_topics_.end()) {
                const auto& node_topics = it->second;
                topics.erase(
                    std::remove_if(topics.begin(), topics.end(),
                        [&node_topics](const TopicInfo& t) {
                            return node_topics.find(t.name) == node_topics.end();
                        }),
                    topics.end());
            }
        }

        // Update scroll list with filtered count
        scroll_list_.setViewportHeight(viewport_h);
        scroll_list_.setItemCount((int)topics.size());
        auto [start, end] = scroll_list_.visibleRange();
        int selected = scroll_list_.selected();

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

            bool is_selected = (idx == selected);
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
        int total = (int)topics.size();
        std::string scroll_info;
        if (total > viewport_h) {
            scroll_info = " [" + std::to_string(start + 1) + "-" + std::to_string(end)
                        + "/" + std::to_string(total) + "]";
        }

        auto content = vbox({
            mode_bar,
            separator(),
            header,
            separator(),
            vbox(std::move(rows)) | flex,
            separator(),
            hbox({
                text(" [a] All  [w] Watched  [f] Node  [/] Search  [Up/Down] Select  [PgUp/PgDn] Scroll" + scroll_info) | dim,
            }),
        });

        // Overlay dropdown if open
        auto dropdown = node_filter_.renderDropdown();
        if (node_filter_.inputActive()) {
            return dbox({content, dropdown | center});
        }

        return content;
    });

    return CatchEvent(renderer, [this](Event event) {
        std::lock_guard lock(mutex_);

        // Dropdown gets first crack
        if (node_filter_.handleEvent(event)) return true;

        // Search bar
        if (search_bar_.handleEvent(event)) return true;

        // Scroll list handles navigation
        if (scroll_list_.handleEvent(event)) return true;

        // Screen-specific keys
        if (event.is_character() && event.character() == "a") {
            show_all_ = true;
            return true;
        }
        if (event.is_character() && event.character() == "w") {
            show_all_ = false;
            return true;
        }
        return false;
    });
}

void TopicScreen::tick() {
    auto topics = topic_mon_->snapshot();

    // Ensure node data is fresh (refresh is internally throttled to 2s)
    node_inspector_->refresh();
    auto nodes = node_inspector_->nodes();
    std::vector<std::string> node_names;
    std::unordered_map<std::string, std::set<std::string>> node_topics;

    node_names.reserve(nodes.size());
    for (const auto& n : nodes) {
        node_names.push_back(n.full_name);
        std::set<std::string> topics_set;
        for (const auto& t : n.publishers) topics_set.insert(t);
        for (const auto& t : n.subscribers) topics_set.insert(t);
        if (!topics_set.empty()) {
            node_topics[n.full_name] = std::move(topics_set);
        }
    }
    std::sort(node_names.begin(), node_names.end());

    std::lock_guard lock(mutex_);
    cached_topics_ = std::move(topics);
    node_filter_.setOptions(node_names);
    cached_node_topics_ = std::move(node_topics);
}

}  // namespace rtl::tui
