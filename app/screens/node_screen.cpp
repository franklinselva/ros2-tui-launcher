#include "screens/node_screen.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

using namespace ftxui;

namespace rtl::tui {

NodeScreen::NodeScreen(NodeInspector* inspector)
    : inspector_(inspector) {}

ftxui::Component NodeScreen::component() {
    auto renderer = Renderer([this] {
        std::lock_guard lock(mutex_);

        scroll_list_.setItemCount((int)cached_nodes_.size());
        int selected = scroll_list_.selected();

        auto header = hbox({
            text("   NODE") | bold | size(WIDTH, EQUAL, 42),
            text("NAMESPACE") | bold | size(WIDTH, EQUAL, 25),
            text("LIFECYCLE") | bold | size(WIDTH, EQUAL, 15),
            text("STATE") | bold | size(WIDTH, EQUAL, 15),
        });

        Elements rows;
        rows.push_back(header);
        rows.push_back(separator());

        int idx = 0;
        for (const auto& node : cached_nodes_) {
            Color lc_color = Color::GrayDark;
            std::string lc_text = "-";
            if (node.is_lifecycle) {
                lc_text = "Yes";
                lc_color = Color::Cyan;
            }

            std::string state_text = node.lifecycle_state.empty() ? "-" : node.lifecycle_state;
            Color state_color = Color::White;
            if (state_text == "active") state_color = Color::Green;
            else if (state_text == "inactive") state_color = Color::Yellow;
            else if (state_text == "unconfigured") state_color = Color::GrayDark;

            bool is_selected = (idx == selected);
            std::string prefix = is_selected ? " > " : "   ";

            auto row = hbox({
                text(prefix + node.name) | size(WIDTH, EQUAL, 42)
                    | (is_selected ? bold : nothing),
                text(node.ns) | dim | size(WIDTH, EQUAL, 25),
                text(lc_text) | color(lc_color) | size(WIDTH, EQUAL, 15),
                text(state_text) | color(state_color) | size(WIDTH, EQUAL, 15),
            });

            if (is_selected) {
                row = row | inverted;
            }

            rows.push_back(row);
            idx++;
        }

        if (cached_nodes_.empty()) {
            rows.push_back(text(" No nodes discovered (is ROS 2 running?)") | dim);
        }

        return vbox({
            vbox(std::move(rows)) | flex,
            separator(),
            hbox({
                text(" " + std::to_string(cached_nodes_.size()) + " nodes  [Up/Down] Select  [r] Refresh") | dim,
            }),
        });
    });

    return CatchEvent(renderer, [this](Event event) {
        std::lock_guard lock(mutex_);

        // Scroll list handles navigation
        if (scroll_list_.handleEvent(event)) return true;

        // Screen-specific keys
        if (event.is_character() && event.character() == "r") {
            inspector_->refresh();
            return true;
        }
        return false;
    });
}

void NodeScreen::tick() {
    // refresh() is internally throttled to 2s intervals
    inspector_->refresh();
    auto nodes = inspector_->nodes();

    std::lock_guard lock(mutex_);
    cached_nodes_ = std::move(nodes);
}

}  // namespace rtl::tui
