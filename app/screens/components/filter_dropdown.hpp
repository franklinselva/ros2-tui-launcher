#pragma once

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace rtl::tui {

/// Dropdown filter selector for TUI screens.
///
/// Press the trigger key to open, then navigate with Up/Down, select with
/// Enter, cancel with Escape.  When open, it captures all events (acts as
/// an input mode).
///
/// No internal mutex. Caller must hold their own lock before calling any method.
class FilterDropdown {
public:
    /// @param trigger  The character key that opens the dropdown (e.g. "f", "n", "v")
    /// @param label    Label shown before the value (e.g. "Source", "Node", "Level")
    /// @param color    Color for the selected value
    explicit FilterDropdown(std::string trigger, std::string label, ftxui::Color color)
        : trigger_(std::move(trigger))
        , label_(std::move(label))
        , color_(color) {}

    /// Set the list of options. Index 0 is always "All" (prepended automatically).
    /// Call this each tick or whenever the options change.
    void setOptions(const std::vector<std::string>& options) {
        options_ = options;
        // Clamp selection if options shrunk
        if (selected_ > (int)options_.size()) {
            selected_ = 0;
        }
    }

    /// Returns true if the event was consumed.
    bool handleEvent(const ftxui::Event& event) {
        if (open_) {
            if (event == ftxui::Event::Escape) {
                open_ = false;
                cursor_ = selected_;  // reset cursor
                return true;
            }
            if (event == ftxui::Event::Return) {
                selected_ = cursor_;
                open_ = false;
                return true;
            }
            if (event == ftxui::Event::ArrowUp || event == ftxui::Event::Character("k")) {
                if (cursor_ > 0) cursor_--;
                return true;
            }
            if (event == ftxui::Event::ArrowDown || event == ftxui::Event::Character("j")) {
                int count = 1 + (int)options_.size();  // "All" + options
                if (cursor_ < count - 1) cursor_++;
                return true;
            }
            // Consume all other events while open
            return true;
        }

        // Trigger key opens the dropdown
        if (event.is_character() && event.character() == trigger_) {
            open_ = true;
            cursor_ = selected_;
            return true;
        }

        return false;
    }

    /// True when the dropdown is open and capturing input.
    bool inputActive() const { return open_; }

    /// Currently selected index (0 = "All", 1+ = options index + 1).
    int selected() const { return selected_; }

    /// The selected option string, or empty if "All".
    std::string selectedValue() const {
        if (selected_ == 0 || selected_ > (int)options_.size()) return "";
        return options_[selected_ - 1];
    }

    /// Render the inline label + value (for the filter bar).
    ftxui::Element renderInline() const {
        using namespace ftxui;
        std::string val = (selected_ == 0) ? "All"
            : (selected_ <= (int)options_.size() ? options_[selected_ - 1] : "All");
        return hbox({
            text(" " + label_ + ": ") | bold,
            text("[" + val + "]") | color(color_),
        });
    }

    /// Render the dropdown overlay (call in the renderer, overlay on top of content).
    /// Returns an empty element if not open.
    ftxui::Element renderDropdown() const {
        using namespace ftxui;
        if (!open_) return text("");

        int count = 1 + (int)options_.size();
        Elements items;
        items.push_back(text(" " + label_ + " Filter ") | bold | inverted);
        items.push_back(separator());

        for (int i = 0; i < count; ++i) {
            std::string name = (i == 0) ? "All" : options_[i - 1];
            bool is_cursor = (i == cursor_);
            bool is_selected = (i == selected_);

            auto item = text((is_cursor ? " > " : "   ") + name
                + (is_selected ? " *" : ""));

            if (is_cursor) {
                item = item | bold | inverted;
            } else if (is_selected) {
                item = item | color(color_);
            } else {
                item = item | dim;
            }
            items.push_back(item);
        }

        items.push_back(separator());
        items.push_back(text(" [Enter] Select  [Esc] Cancel ") | dim);

        return vbox(std::move(items)) | border | bgcolor(Color::Black);
    }

private:
    std::string trigger_;
    std::string label_;
    ftxui::Color color_;
    std::vector<std::string> options_;
    int selected_ = 0;
    int cursor_ = 0;
    bool open_ = false;
};

}  // namespace rtl::tui
