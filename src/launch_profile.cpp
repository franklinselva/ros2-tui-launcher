#include "ros2_tui_launcher/launch_profile.hpp"
#include "ros2_tui_launcher/config_validator.hpp"

#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace rtl {

std::string LaunchEntry::displayName() const {
    if (!name.empty()) return name;
    if (!launch_file.empty()) return package + "/" + launch_file;
    return package + "/" + executable;
}

LaunchProfile loadProfile(const std::filesystem::path& path) {
    LaunchProfile profile;

    auto root = YAML::LoadFile(path.string());

    profile.name = root["name"].as<std::string>(path.stem().string());
    profile.description = root["description"].as<std::string>("");
    profile.topic_hz_rate = root["topic_hz_rate"].as<double>(1.0);

    // Parse launch entries
    if (auto launch_node = root["launch"]) {
        for (const auto& entry_node : launch_node) {
            LaunchEntry entry;
            entry.name = entry_node["name"].as<std::string>("");
            entry.package = entry_node["package"].as<std::string>("");
            entry.launch_file = entry_node["launch_file"].as<std::string>("");
            entry.executable = entry_node["executable"].as<std::string>("");
            entry.autostart = entry_node["autostart"].as<bool>(true);
            entry.restart_policy = entry_node["restart_policy"].as<std::string>("never");

            if (auto args_node = entry_node["args"]) {
                for (auto it = args_node.begin(); it != args_node.end(); ++it) {
                    entry.args[it->first.as<std::string>()] = it->second.as<std::string>();
                }
            }

            if (auto env_node = entry_node["env"]) {
                for (auto it = env_node.begin(); it != env_node.end(); ++it) {
                    entry.env[it->first.as<std::string>()] = it->second.as<std::string>();
                }
            }

            entry.params_file = entry_node["params_file"].as<std::string>("");

            if (auto params_node = entry_node["parameters"]) {
                for (auto it = params_node.begin(); it != params_node.end(); ++it) {
                    entry.parameters[it->first.as<std::string>()] = it->second.as<std::string>();
                }
            }

            if (entry.package.empty()) {
                spdlog::warn("Launch entry missing 'package' in {}", path.string());
                continue;
            }
            if (entry.launch_file.empty() && entry.executable.empty()) {
                spdlog::warn("Launch entry needs 'launch_file' or 'executable' in {}", path.string());
                continue;
            }

            profile.entries.push_back(std::move(entry));
        }
    }

    // Parse monitored topics
    if (auto monitor_node = root["monitor"]) {
        if (auto topics_node = monitor_node["topics"]) {
            for (const auto& t : topics_node) {
                MonitoredTopic mt;
                if (t.IsScalar()) {
                    mt.topic = t.as<std::string>();
                } else {
                    mt.topic = t["name"].as<std::string>("");
                    mt.expected_hz = t["expected_hz"].as<double>(0.0);
                }
                if (!mt.topic.empty()) {
                    profile.monitored_topics.push_back(std::move(mt));
                }
            }
        }
        if (auto rate = monitor_node["hz_rate"]) {
            profile.topic_hz_rate = rate.as<double>(1.0);
        }
    }

    // Parse logging configuration
    if (auto log_node = root["logging"]) {
        profile.log_config.log_dir = log_node["log_dir"].as<std::string>("");
        auto max_mb = log_node["max_file_size_mb"].as<size_t>(10);
        profile.log_config.max_file_size_bytes = max_mb * 1024 * 1024;
        profile.log_config.max_rotated_files = log_node["max_rotated_files"].as<size_t>(5);
        auto flush_ms = log_node["flush_interval_ms"].as<int>(1000);
        profile.log_config.flush_interval = std::chrono::milliseconds(flush_ms);
    }

    // Validate the parsed profile
    ConfigValidator validator;
    auto vresult = validator.validate(profile, path);

    // Remove invalid entries in reverse order to preserve indices
    for (auto it = vresult.invalid_entry_indices.rbegin();
         it != vresult.invalid_entry_indices.rend(); ++it) {
        if (*it < profile.entries.size()) {
            profile.entries.erase(profile.entries.begin() +
                                  static_cast<std::ptrdiff_t>(*it));
        }
    }

    if (!vresult.valid) {
        throw std::runtime_error(
            "Profile '" + profile.name + "' has critical validation errors");
    }

    spdlog::info("Loaded profile '{}' with {} entries, {} monitored topics",
                 profile.name, profile.entries.size(), profile.monitored_topics.size());
    return profile;
}

std::vector<LaunchProfile> discoverProfiles(const std::filesystem::path& dir) {
    std::vector<LaunchProfile> profiles;

    if (!std::filesystem::exists(dir)) {
        spdlog::warn("Profile directory does not exist: {}", dir.string());
        return profiles;
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ".yaml" || entry.path().extension() == ".yml") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    for (const auto& f : files) {
        try {
            profiles.push_back(loadProfile(f));
        } catch (const std::exception& e) {
            spdlog::error("Failed to load profile {}: {}", f.string(), e.what());
        }
    }

    return profiles;
}

}  // namespace rtl
