#pragma once

#include <string>
#include <ftxui/component/component.hpp>

namespace rtl::tui {

/// Base interface for all TUI screens.
class Screen {
public:
    virtual ~Screen() = default;

    /// Display name shown in the tab bar
    virtual std::string name() const = 0;

    /// Short key hint (e.g. "L", "G", "T", "N")
    virtual std::string hotkey() const = 0;

    /// FTXUI component tree for this screen
    virtual ftxui::Component component() = 0;

    /// Called periodically (~30 Hz) from the tick thread.
    virtual void tick() {}
};

}  // namespace rtl::tui
