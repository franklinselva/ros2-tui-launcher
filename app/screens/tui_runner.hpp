#pragma once

#include "screen.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace rtl::tui {

/// Generic TUI runner that manages screens, tab bar, tick thread, and FTXUI loop.
/// No dependency on any specific domain — just screens.
class TuiRunner {
public:
    /// @param title  Header title shown in the TUI
    explicit TuiRunner(std::string title);
    ~TuiRunner();

    TuiRunner(const TuiRunner&) = delete;
    TuiRunner& operator=(const TuiRunner&) = delete;

    /// Add a screen to the tab bar. Call before run().
    void addScreen(std::shared_ptr<Screen> screen);

    /// Convenience: construct a screen in-place and add it.
    template <typename T, typename... Args>
    void addScreen(Args&&... args) {
        addScreen(std::make_shared<T>(std::forward<Args>(args)...));
    }

    /// Set a status text shown in the header (thread-safe).
    void setStatus(const std::string& status);

    /// Run the TUI (blocks until user quits with Q or requestStop() is called).
    void run();

    /// Request the TUI to stop (thread-safe).
    void requestStop();

    bool running() const { return running_.load(); }

private:
    std::string buildHotkeyHint() const;

    std::string title_;
    std::vector<std::shared_ptr<Screen>> screens_;
    std::atomic<bool> running_{false};
    ftxui::ScreenInteractive* active_screen_ = nullptr;

    mutable std::mutex status_mutex_;
    std::string status_text_ = "Ready";
};

}  // namespace rtl::tui
