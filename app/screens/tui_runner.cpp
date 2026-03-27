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

    // Global key handler — wraps the outside.
    // FTXUI's CatchEvent on the outside intercepts FIRST.
    // We only handle quit and tab-switch keys here. All other events
    // fall through to FTXUI's normal component tree propagation (returning false).
    auto with_global_keys = CatchEvent(main_renderer, [&](Event event) -> bool {
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
            char ch_upper = static_cast<char>(std::toupper(ch[0]));
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

        // Let FTXUI propagate to inner components (screen-level CatchEvent handlers)
        return false;
    });

    auto screen = ScreenInteractive::Fullscreen();
    active_screen_ = &screen;

    // Background tick thread (~30 Hz) — only ticks the active screen
    std::thread tick_thread([&] {
        while (running_.load()) {
            int active = selected_tab;
            if (active >= 0 && active < (int)screens_.size()) {
                screens_[active]->tick();
            }
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
        hint += "[" + std::to_string(i + 1) + "/" + screens_[i]->hotkey() + "] "
             + screens_[i]->name() + "  ";
    }
    return hint;
}

}  // namespace rtl::tui
