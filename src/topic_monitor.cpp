#include "ros2_tui_launcher/topic_monitor.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <numeric>

namespace rtl {

TopicMonitor::TopicMonitor(
    rclcpp::Node::SharedPtr node,
    std::chrono::milliseconds poll_interval)
    : node_(node), poll_interval_(poll_interval) {}

TopicMonitor::~TopicMonitor() {
    stop();
}

void TopicMonitor::setWatchedTopics(
    const std::vector<std::pair<std::string, double>>& topics)
{
    std::lock_guard lock(mutex_);

    // Remove old subscriptions for topics no longer watched
    std::set<std::string> new_topics;
    for (const auto& [name, _] : topics) {
        new_topics.insert(name);
    }
    for (auto it = hz_trackers_.begin(); it != hz_trackers_.end();) {
        if (new_topics.find(it->first) == new_topics.end()) {
            it = hz_trackers_.erase(it);
        } else {
            ++it;
        }
    }

    // Add new subscriptions
    for (const auto& [topic_name, expected_hz] : topics) {
        if (hz_trackers_.count(topic_name)) {
            hz_trackers_[topic_name].expected_hz = expected_hz;
            continue;
        }

        HzTracker tracker;
        tracker.expected_hz = expected_hz;

        // Find topic type from the graph
        auto topic_types = node_->get_topic_names_and_types();
        std::string topic_type;
        if (auto it = topic_types.find(topic_name); it != topic_types.end() && !it->second.empty()) {
            topic_type = it->second[0];
        }

        if (!topic_type.empty()) {
            try {
                tracker.subscription = node_->create_generic_subscription(
                    topic_name, topic_type,
                    rclcpp::SensorDataQoS(),
                    [this, topic_name](std::shared_ptr<rclcpp::SerializedMessage>) {
                        std::lock_guard lock(mutex_);
                        if (auto it = hz_trackers_.find(topic_name); it != hz_trackers_.end()) {
                            it->second.timestamps.push_back(std::chrono::steady_clock::now());
                            while (it->second.timestamps.size() > HzTracker::kMaxSamples) {
                                it->second.timestamps.pop_front();
                            }
                        }
                    });
                spdlog::debug("Subscribed to '{}' [{}] for Hz monitoring", topic_name, topic_type);
            } catch (const std::exception& e) {
                spdlog::warn("Failed to subscribe to '{}': {}", topic_name, e.what());
            }
        } else {
            spdlog::debug("Topic '{}' not yet advertised, will retry", topic_name);
        }

        hz_trackers_[topic_name] = std::move(tracker);
    }
}

void TopicMonitor::start() {
    if (running_.load()) return;
    running_.store(true);
    poll_thread_ = std::thread(&TopicMonitor::pollLoop, this);
}

void TopicMonitor::stop() {
    running_.store(false);
    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }
}

std::vector<TopicInfo> TopicMonitor::snapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<TopicInfo> result;
    result.reserve(topics_.size());
    for (const auto& [_, info] : topics_) {
        result.push_back(info);
    }
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    return result;
}

void TopicMonitor::pollLoop() {
    while (running_.load()) {
        refreshTopicList();

        // Compute Hz for watched topics
        {
            std::lock_guard lock(mutex_);
            for (auto& [name, tracker] : hz_trackers_) {
                double hz = computeHz(tracker);
                auto& info = topics_[name];
                info.name = name;
                info.hz = hz;
                info.expected_hz = tracker.expected_hz;
                info.stale = (tracker.expected_hz > 0 && hz < tracker.expected_hz * 0.5);
                info.last_updated = std::chrono::steady_clock::now();

                // Retry subscription if it was null (topic wasn't advertised before)
                if (!tracker.subscription) {
                    auto topic_types = node_->get_topic_names_and_types();
                    if (auto it = topic_types.find(name);
                        it != topic_types.end() && !it->second.empty()) {
                        try {
                            tracker.subscription = node_->create_generic_subscription(
                                name, it->second[0],
                                rclcpp::SensorDataQoS(),
                                [this, name](std::shared_ptr<rclcpp::SerializedMessage>) {
                                    std::lock_guard lock(mutex_);
                                    if (auto it2 = hz_trackers_.find(name); it2 != hz_trackers_.end()) {
                                        it2->second.timestamps.push_back(std::chrono::steady_clock::now());
                                        while (it2->second.timestamps.size() > HzTracker::kMaxSamples) {
                                            it2->second.timestamps.pop_front();
                                        }
                                    }
                                });
                        } catch (...) {}
                    }
                }
            }
        }

        std::this_thread::sleep_for(poll_interval_);
    }
}

void TopicMonitor::refreshTopicList() {
    auto topic_types = node_->get_topic_names_and_types();

    std::lock_guard lock(mutex_);
    for (const auto& [name, types] : topic_types) {
        auto& info = topics_[name];
        info.name = name;
        if (!types.empty()) {
            info.type = types[0];
        }
        info.publisher_count = node_->count_publishers(name);
        info.subscriber_count = node_->count_subscribers(name);
        info.last_updated = std::chrono::steady_clock::now();
    }
}

double TopicMonitor::computeHz(const HzTracker& tracker) const {
    if (tracker.timestamps.size() < 2) return 0.0;

    // Only consider messages from the last 5 seconds
    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - std::chrono::seconds(5);

    std::vector<double> intervals;
    for (size_t i = 1; i < tracker.timestamps.size(); ++i) {
        if (tracker.timestamps[i] < cutoff) continue;
        auto dt = std::chrono::duration<double>(
            tracker.timestamps[i] - tracker.timestamps[i - 1]);
        intervals.push_back(dt.count());
    }

    if (intervals.empty()) return 0.0;

    double avg_interval = std::accumulate(intervals.begin(), intervals.end(), 0.0)
                          / intervals.size();
    return (avg_interval > 0) ? 1.0 / avg_interval : 0.0;
}

}  // namespace rtl
