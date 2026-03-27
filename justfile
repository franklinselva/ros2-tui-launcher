# ros2-tui-launcher justfile
# Usage: just <recipe>

# Default workspace root (override with: just --set ws_root /path/to/ws)
ws_root := env("ROS_WS", parent_directory(justfile_directory()) + "/../..")

# Build type
build_type := env("CMAKE_BUILD_TYPE", "Release")

# ── Build ────────────────────────────────────────────────────────────────────

# Build the package with colcon
build:
    cd {{ws_root}} && colcon build --packages-select ros2_tui_launcher --cmake-args -DCMAKE_BUILD_TYPE={{build_type}} --symlink-install

# Build with tests enabled
build-test:
    cd {{ws_root}} && colcon build --packages-select ros2_tui_launcher --cmake-args -DCMAKE_BUILD_TYPE=Debug -DRTL_BUILD_TESTS=ON --symlink-install

# Build in debug mode
build-debug:
    cd {{ws_root}} && colcon build --packages-select ros2_tui_launcher --cmake-args -DCMAKE_BUILD_TYPE=Debug --symlink-install

# Clean build artifacts
clean:
    cd {{ws_root}} && colcon build --packages-select ros2_tui_launcher --cmake-args -DCMAKE_BUILD_TYPE={{build_type}} --cmake-clean-first --symlink-install

# ── Run ──────────────────────────────────────────────────────────────────────

# Run the TUI launcher (source workspace first)
run *ARGS:
    cd {{ws_root}} && source install/setup.bash && ros2 run ros2_tui_launcher ros2-tui-launcher {{ARGS}}

# Run with a specific profile directory
run-profile dir:
    cd {{ws_root}} && source install/setup.bash && ros2 run ros2_tui_launcher ros2-tui-launcher --profiles {{dir}}

# Run with a specific config file
run-config file:
    cd {{ws_root}} && source install/setup.bash && ros2 run ros2_tui_launcher ros2-tui-launcher --config {{file}}

# ── Test ─────────────────────────────────────────────────────────────────────

# Run all tests for this package
test: build-test
    cd {{ws_root}} && colcon test --packages-select ros2_tui_launcher --event-handlers console_direct+

# Run tests and show results
test-results:
    cd {{ws_root}} && colcon test-result --verbose --test-result-base build/ros2_tui_launcher

# ── Lint / Analysis ──────────────────────────────────────────────────────────

# Run clang-tidy on the package
lint:
    cd {{ws_root}} && colcon build --packages-select ros2_tui_launcher --cmake-args -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON --symlink-install
    run-clang-tidy -p {{ws_root}}/build/ros2_tui_launcher -header-filter='.*ros2-tui-launcher.*'

# ── Utility ──────────────────────────────────────────────────────────────────

# Build and run in one step
br *ARGS: build
    just run {{ARGS}}

# Show the workspace root being used
info:
    @echo "Workspace root: {{ws_root}}"
    @echo "Build type:     {{build_type}}"
    @echo "Justfile dir:   {{justfile_directory()}}"
