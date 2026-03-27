#include "screens/tui_runner.hpp"
#include "screens/launch_screen.hpp"
#include "screens/log_screen.hpp"
#include "screens/topic_screen.hpp"
#include "screens/node_screen.hpp"
#include "screens/parameter_screen.hpp"

#include "ros2_tui_launcher/launch_profile.hpp"
#include "ros2_tui_launcher/process_manager.hpp"
#include "ros2_tui_launcher/log_aggregator.hpp"
#include "ros2_tui_launcher/topic_monitor.hpp"
#include "ros2_tui_launcher/node_inspector.hpp"
#include "ros2_tui_launcher/parameter_manager.hpp"
#include "ros2_tui_launcher/system_monitor.hpp"
#include "ros2_tui_launcher/log_writer.hpp"

#include <rclcpp/rclcpp.hpp>
#include <spdlog/spdlog.h>

#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>

namespace {
// Forward-declared so the signal handler can request TUI exit
std::atomic<rtl::tui::TuiRunner*> g_tui{nullptr};

void signalHandler(int) {
    auto* tui = g_tui.load(std::memory_order_acquire);
    if (tui) {
        tui->requestStop();
    }
}
}  // namespace

int main(int argc, char* argv[]) {
    // Parse CLI arguments — separate our flags from ROS args
    std::string profile_dir = ".";
    std::string profile_file;

    // Collect args that are NOT ours, to pass to rclcpp
    std::vector<const char*> ros_argv;
    ros_argv.push_back(argv[0]);

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--profiles" || arg == "-p") && i + 1 < argc) {
            profile_dir = argv[++i];
        } else if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            profile_file = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "ros2-tui-launcher - ROS 2 Launch TUI Manager\n\n"
                      << "Usage:\n"
                      << "  ros2-tui-launcher [OPTIONS]\n\n"
                      << "Options:\n"
                      << "  -p, --profiles <DIR>   Directory containing launch profile YAML files\n"
                      << "  -c, --config <FILE>    Single profile YAML file to load\n"
                      << "  -h, --help             Show this help\n\n"
                      << "Hotkeys:\n"
                      << "  [L] Launch  [G] Logs  [T] Topics  [N] Nodes  [P] Params  [Q] Quit\n";
            return 0;
        } else {
            // Pass unrecognized args (including --ros-args) to rclcpp
            ros_argv.push_back(argv[i]);
        }
    }

    // Initialize ROS 2 with only ROS-relevant args.
    // Disable rclcpp's built-in signal handlers — we manage SIGINT/SIGTERM
    // ourselves so they don't conflict with FTXUI's terminal raw mode.
    rclcpp::InitOptions init_options;
    init_options.shutdown_on_signal = false;
    int ros_argc = static_cast<int>(ros_argv.size());
    rclcpp::init(ros_argc, const_cast<char**>(ros_argv.data()), init_options);
    auto node = rclcpp::Node::make_shared("ros2_tui_launcher");

    // Load profiles
    std::vector<rtl::LaunchProfile> profiles;
    if (!profile_file.empty()) {
        try {
            profiles.push_back(rtl::loadProfile(profile_file));
        } catch (const std::exception& e) {
            spdlog::error("Failed to load profile '{}': {}", profile_file, e.what());
            return 1;
        }
    } else {
        profiles = rtl::discoverProfiles(profile_dir);
    }

    if (profiles.empty()) {
        spdlog::warn("No profiles found. Use --profiles <dir> or --config <file>.");
        spdlog::info("Creating empty default profile.");
        rtl::LaunchProfile empty;
        empty.name = "Empty";
        empty.description = "No profile loaded";
        profiles.push_back(std::move(empty));
    }

    int active_profile = 0;

    // Create core components
    rtl::ProcessManager proc_mgr;
    rtl::LogAggregator log_agg(node);
    rtl::TopicMonitor topic_mon(node);
    rtl::NodeInspector node_inspector(node);
    rtl::ParameterManager param_mgr(node);
    rtl::SystemMonitor sys_mon;

    // Create log writer for file persistence
    auto session_ts = rtl::LogWriter::sessionTimestamp();
    auto log_writer = std::make_shared<rtl::LogWriter>(
        profiles[active_profile].log_config,
        profiles[active_profile].name,
        session_ts);
    log_agg.setLogWriter(log_writer);

    // Wire process output into log aggregator
    proc_mgr.setLogCallback([&log_agg](const std::string& source, const std::string& line) {
        log_agg.pushRaw(source, line);
    });

    // Set watched topics from active profile
    if (!profiles[active_profile].monitored_topics.empty()) {
        std::vector<std::pair<std::string, double>> watched;
        for (const auto& mt : profiles[active_profile].monitored_topics) {
            watched.emplace_back(mt.topic, mt.expected_hz);
        }
        topic_mon.setWatchedTopics(watched);
    }

    // Start monitors
    topic_mon.start();

    // Spin rclcpp in a background thread
    std::thread spin_thread([&node] {
        rclcpp::spin(node);
    });

    // Auto-start processes marked with autostart
    for (const auto& entry : profiles[active_profile].entries) {
        if (entry.autostart) {
            proc_mgr.start(entry);
        }
    }

    // Build and run TUI
    rtl::tui::TuiRunner tui("ros2-tui-launcher");
    g_tui.store(&tui, std::memory_order_release);

    // Install signal handlers AFTER creating TUI so they can call requestStop()
    // Use sigaction instead of std::signal for thread-safe signal handling
    struct sigaction sa{};
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    auto launch_screen = std::make_shared<rtl::tui::LaunchScreen>(
        &profiles, &active_profile, &proc_mgr, &sys_mon);
    launch_screen->setProfileChangeCallback(
        [&profiles, &log_agg, &log_writer, &session_ts](int new_idx) {
            // Swap log writer for the new profile (same session timestamp)
            log_writer = std::make_shared<rtl::LogWriter>(
                profiles[new_idx].log_config,
                profiles[new_idx].name,
                session_ts);
            log_agg.setLogWriter(log_writer);
        });
    tui.addScreen(launch_screen);
    tui.addScreen<rtl::tui::LogScreen>(&log_agg, &node_inspector);
    tui.addScreen<rtl::tui::TopicScreen>(&topic_mon, &node_inspector);
    tui.addScreen<rtl::tui::NodeScreen>(&node_inspector);
    tui.addScreen<rtl::tui::ParameterScreen>(&node_inspector, &param_mgr);

    // Run TUI (blocks) — wrapped in try/catch for clean shutdown on exceptions
    try {
        tui.run();
    } catch (const std::exception& e) {
        spdlog::error("TUI error: {}", e.what());
    }

    g_tui.store(nullptr, std::memory_order_release);

    // Cleanup
    spdlog::info("Shutting down...");
    topic_mon.stop();
    proc_mgr.stopAll();
    rclcpp::shutdown();
    if (spin_thread.joinable()) {
        spin_thread.join();
    }

    return 0;
}
