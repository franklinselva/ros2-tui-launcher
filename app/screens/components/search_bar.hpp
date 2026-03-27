#pragma once

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <string>

namespace rtl::tui {

/// Reusable search bar component for TUI screens.
///
/// Handles: "/" to enter search mode, Escape/Return to exit, character
/// accumulation, backspace deletion.
///
/// No internal mutex. Caller must hold their own lock before calling any method.
class SearchBar {
public:
    /// Returns true if the event was consumed.
    bool handleEvent(const ftxui::Event& event) {
        if (active_) {
            if (event == ftxui::Event::Escape || event == ftxui::Event::Return) {
                active_ = false;
                return true;
            }
            if (event == ftxui::Event::Backspace) {
                if (!text_.empty()) text_.pop_back();
                return true;
            }
            if (event.is_character()) {
                text_ += event.character();
                return true;
            }
            // Let non-character events (arrows, mouse, etc.) pass through
            return false;
        }

        // Not active — "/" enters search mode
        if (event.is_character() && event.character() == "/") {
            active_ = true;
            text_.clear();
            return true;
        }

        return false;
    }

    /// True when the search bar is actively capturing text input.
    /// Used by the global handler to skip character-based hotkeys.
    bool inputActive() const { return active_; }

    /// Current search query text (may be empty).
    const std::string& query() const { return text_; }

    /// Clear search text and exit search mode.
    void clear() {
        active_ = false;
        text_.clear();
    }

    /// Render the search bar as an Element for embedding in an hbox.
    ftxui::Element render() const {
        using namespace ftxui;
        auto search_display = active_
            ? text(text_ + "_") | color(Color::White)
            : (text_.empty() ? text("(none)") | dim : text(text_) | dim);

        auto hint = active_
            ? text(" [ESC] cancel ") | color(Color::Yellow)
            : text("");

        return hbox({
            text("  Search: ") | bold,
            search_display,
            hint,
        });
    }

private:
    bool active_ = false;
    std::string text_;
};

}  // namespace rtl::tui
