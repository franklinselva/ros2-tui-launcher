#include "screens/tui_runner.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <thread>

using namespace ftxui;

namespace rtl::tui {

TuiRunner::TuiRunner(std::string title)
    : title_(std::move(title)) {}

TuiRunner::~TuiRunner() {
    requestStop();
}

void TuiRunner::addScreen(std::shared_ptr<Screen> screen) {
    screens_.push_back(std::move(screen));
}

void TuiRunner::setStatus(const std::string& status) {
    std::lock_guard lock(status_mutex_);
    status_text_ = status;
}

void TuiRunner::requestStop() {
    running_.store(false);
    if (active_screen_) {
        active_screen_->Exit();
    }
}

void TuiRunner::run() {
    if (screens_.empty()) return;

    running_.store(true);

    // Tab bar entries
    std::vector<std::string> tab_names;
    for (const auto& s : screens_)
        tab_names.push_back(s->name());

    int selected_tab = 0;

    auto tab_toggle = Toggle(&tab_names, &selected_tab);

    // Screen components already have CatchEvent wired in — they handle their own keys
    std::vector<Component> screen_components;
    for (auto& s : screens_)
        screen_components.push_back(s->component());
    auto tab_content = Container::Tab(std::move(screen_components), &selected_tab);

    // Layout: tab bar on top, content below.
    // Use Container::Vertical so focus flows to tab_content (the active screen).
    auto main_component = Container::Vertical({
        tab_toggle,
        tab_content,
    });

    // Render the chrome (header, border, hotkey bar) around the component tree
    auto main_renderer = Renderer(main_component, [&] {
        std::string status;
        {
            std::lock_guard lock(status_mutex_);
            status = status_text_;
        }

        return vbox({
            hbox({
                text(" " + title_ + " ") | bold | color(Color::Cyan),
                filler(),
                text(" " + status + " ") | color(Color::Green),
            }),
            tab_toggle->Render() | borderLight,
            tab_content->Render() | flex,
            hbox({
                text(" " + buildHotkeyHint() + " [Q]uit ") | dim,
            }),
        }) | border;
    });

    // Global key handler — only catches Q (quit) and tab hotkeys (uppercase).
    // This wraps the outside, but screen-level CatchEvent components are inside
    // main_component and get events via FTXUI's component tree propagation.
    // However, CatchEvent on the outside intercepts FIRST in FTXUI.
    //
    // Solution: Use a two-pass approach. First let the inner component try to handle
    // the event. If it didn't handle it, then check for global keys.
    auto with_global_keys = CatchEvent(main_renderer, [&](Event event) -> bool {
        // First, let the active screen component try to handle the event.
        // We do this by forwarding to tab_content. If it returns true, it handled it.
        bool handled = tab_content->OnEvent(event);
        if (handled) return true;

        // Quit (q or Q)
        if (event.is_character() &&
            (event.character() == "q" || event.character() == "Q")) {
            running_.store(false);
            auto screen = ScreenInteractive::Active();
            if (screen) screen->Exit();
            return true;
        }

        // Tab switching — match both upper and lowercase hotkeys
        if (event.is_character()) {
            std::string ch = event.character();
            char ch_upper = std::toupper(ch[0]);
            for (int i = 0; i < (int)screens_.size(); i++) {
                auto hk = screens_[i]->hotkey();
                if (!hk.empty() && ch_upper == std::toupper(hk[0])) {
                    selected_tab = i;
                    return true;
                }
            }
        }

        // Number keys 1-9 for quick tab switch
        if (event.is_character()) {
            char c = event.character()[0];
            if (c >= '1' && c <= '9') {
                int idx = c - '1';
                if (idx < (int)screens_.size()) {
                    selected_tab = idx;
                    return true;
                }
            }
        }

        return false;
    });

    auto screen = ScreenInteractive::Fullscreen();
    active_screen_ = &screen;

    // Background tick thread (~30 Hz)
    std::thread tick_thread([&] {
        while (running_.load()) {
            for (auto& s : screens_)
                s->tick();
            screen.Post(Event::Custom);
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
    });

    screen.Loop(with_global_keys);

    running_.store(false);
    active_screen_ = nullptr;
    tick_thread.join();
}

std::string TuiRunner::buildHotkeyHint() const {
    std::string hint;
    for (size_t i = 0; i < screens_.size(); i++) {
        hint += "[" + std::to_string(i + 1) + "/" + screens_[i]->hotkey() + "]"
             + screens_[i]->name() + "  ";
    }
    return hint;
}

}  // namespace rtl::tui
