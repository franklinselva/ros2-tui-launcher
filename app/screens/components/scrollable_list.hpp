#pragma once

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>

#include <algorithm>

namespace rtl::tui {

/// Reusable scroll/selection component for TUI screens.
///
/// Two modes:
///   selection_mode=true  — tracks a selected item, viewport follows selection
///   selection_mode=false — offset-only scrolling (e.g. log viewer), supports auto-scroll
///
/// No internal mutex. Caller must hold their own lock before calling any method.
class ScrollableList {
public:
    struct Config {
        bool selection_mode;
        int mouse_wheel_lines;
        bool auto_scroll_support;

        Config(bool sel = true, int wheel = 3, bool auto_scroll = false)
            : selection_mode(sel), mouse_wheel_lines(wheel), auto_scroll_support(auto_scroll) {}
    };

    explicit ScrollableList(Config config = Config{})
        : config_(config)
        , auto_scroll_(config.auto_scroll_support) {}

    // -- State setters (call each render frame) --

    void setViewportHeight(int h) { viewport_height_ = std::max(1, h); }
    void setItemCount(int count) { item_count_ = std::max(0, count); }

    // -- Read state --

    int selected() const { return selected_; }
    int scrollOffset() const { return scroll_offset_; }
    bool autoScroll() const { return auto_scroll_; }
    int viewportHeight() const { return viewport_height_; }

    void toggleAutoScroll() { auto_scroll_ = !auto_scroll_; }

    struct ViewRange { int start; int end; };

    ViewRange visibleRange() const {
        int total = item_count_;
        int max_start = std::max(0, total - viewport_height_);

        int start;
        if (!config_.selection_mode && auto_scroll_) {
            start = max_start;
        } else {
            start = std::clamp(scroll_offset_, 0, max_start);
        }
        int end = std::min(start + viewport_height_, total);
        return {start, end};
    }

    // -- Event handling --

    bool handleEvent(ftxui::Event event) {
        // Keyboard: up
        if (event == ftxui::Event::ArrowUp || event == ftxui::Event::Character("k")) {
            if (config_.selection_mode) {
                selectUp(1);
            } else {
                scrollUp(1);
            }
            return true;
        }

        // Keyboard: down
        if (event == ftxui::Event::ArrowDown || event == ftxui::Event::Character("j")) {
            if (config_.selection_mode) {
                selectDown(1);
            } else {
                scrollDown(1);
            }
            return true;
        }

        // Page up
        if (event == ftxui::Event::PageUp) {
            if (config_.selection_mode) {
                selectUp(viewport_height_);
            } else {
                scrollUp(viewport_height_);
            }
            return true;
        }

        // Page down
        if (event == ftxui::Event::PageDown) {
            if (config_.selection_mode) {
                selectDown(viewport_height_);
            } else {
                scrollDown(viewport_height_);
            }
            return true;
        }

        // Home
        if (event == ftxui::Event::Home) {
            if (config_.selection_mode) {
                selected_ = 0;
                scroll_offset_ = 0;
            } else {
                auto_scroll_ = false;
                scroll_offset_ = 0;
            }
            return true;
        }

        // End
        if (event == ftxui::Event::End) {
            if (config_.selection_mode) {
                selected_ = std::max(0, item_count_ - 1);
                ensureSelectedVisible();
            } else {
                if (config_.auto_scroll_support) {
                    auto_scroll_ = true;
                }
                scroll_offset_ = std::max(0, item_count_ - viewport_height_);
            }
            return true;
        }

        // Mouse wheel
        if (event.is_mouse()) {
            auto& mouse = event.mouse();
            if (mouse.button == ftxui::Mouse::WheelUp) {
                if (config_.selection_mode) {
                    selectUp(config_.mouse_wheel_lines);
                } else {
                    scrollUp(config_.mouse_wheel_lines);
                }
                return true;
            }
            if (mouse.button == ftxui::Mouse::WheelDown) {
                if (config_.selection_mode) {
                    selectDown(config_.mouse_wheel_lines);
                } else {
                    scrollDown(config_.mouse_wheel_lines);
                }
                return true;
            }
        }

        return false;
    }

private:
    void selectUp(int lines) {
        selected_ = std::max(0, selected_ - lines);
        if (selected_ < scroll_offset_) {
            scroll_offset_ = selected_;
        }
    }

    void selectDown(int lines) {
        selected_ = std::min(std::max(0, item_count_ - 1), selected_ + lines);
        ensureSelectedVisible();
    }

    void scrollUp(int lines) {
        auto_scroll_ = false;
        scroll_offset_ = std::max(0, scroll_offset_ - lines);
    }

    void scrollDown(int lines) {
        auto_scroll_ = false;
        int max_offset = std::max(0, item_count_ - viewport_height_);
        scroll_offset_ = std::min(scroll_offset_ + lines, max_offset);
    }

    void ensureSelectedVisible() {
        if (selected_ >= scroll_offset_ + viewport_height_) {
            scroll_offset_ = selected_ - viewport_height_ + 1;
        }
        if (selected_ < scroll_offset_) {
            scroll_offset_ = selected_;
        }
    }

    Config config_;
    int selected_ = 0;
    int scroll_offset_ = 0;
    int item_count_ = 0;
    int viewport_height_ = 20;
    bool auto_scroll_ = false;
};

}  // namespace rtl::tui
