#include "ros2_tui_launcher/topic_monitor.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace rtl {

TopicMonitor::TopicMonitor(
    rclcpp::Node::SharedPtr node,
    std::chrono::milliseconds poll_interval)
    : node_(node), poll_interval_(poll_interval) {}

TopicMonitor::~TopicMonitor() {
    stop();
    // Clear subscriptions before node is potentially destroyed
    std::lock_guard lock(mutex_);
    hz_trackers_.clear();
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
        tracker.incoming = std::make_unique<rigtorp::SPSCQueue<
            std::chrono::steady_clock::time_point>>(HzTracker::kQueueCapacity);

        tryCreateSubscription(topic_name, tracker);
        hz_trackers_[topic_name] = std::move(tracker);
    }
}

void TopicMonitor::tryCreateSubscription(const std::string& name, HzTracker& tracker) {
    // Query topic type outside lock-sensitive path
    auto topic_types = node_->get_topic_names_and_types();
    std::string topic_type;
    if (auto it = topic_types.find(name); it != topic_types.end() && !it->second.empty()) {
        topic_type = it->second[0];
    }

    if (topic_type.empty()) {
        spdlog::debug("Topic '{}' not yet advertised, will retry", name);
        return;
    }

    try {
        auto* queue_ptr = tracker.incoming.get();
        tracker.subscription = node_->create_generic_subscription(
            name, topic_type,
            rclcpp::SensorDataQoS(),
            [queue_ptr](std::shared_ptr<rclcpp::SerializedMessage>) {
                // Lock-free push — if queue is full, drop oldest sample
                queue_ptr->try_push(std::chrono::steady_clock::now());
            });
        spdlog::debug("Subscribed to '{}' [{}] for Hz monitoring", name, topic_type);
    } catch (const std::exception& e) {
        spdlog::warn("Failed to subscribe to '{}': {}", name, e.what());
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

        // Drain SPSCQueues and compute Hz for watched topics
        {
            std::lock_guard lock(mutex_);
            for (auto& [name, tracker] : hz_trackers_) {
                // Drain lock-free queue into local timestamp deque
                auto* ts_ptr = tracker.incoming->front();
                while (ts_ptr != nullptr) {
                    tracker.timestamps.push_back(*ts_ptr);
                    tracker.incoming->pop();
                    ts_ptr = tracker.incoming->front();
                }
                while (tracker.timestamps.size() > HzTracker::kMaxSamples) {
                    tracker.timestamps.pop_front();
                }

                double hz = computeHz(tracker);
                auto& info = topics_[name];
                info.name = name;
                info.hz = hz;
                info.expected_hz = tracker.expected_hz;
                info.stale = (tracker.expected_hz > 0 && hz < tracker.expected_hz * 0.5);
                info.last_updated = std::chrono::steady_clock::now();

                // Retry subscription if it was null (topic wasn't advertised before)
                if (!tracker.subscription) {
                    tryCreateSubscription(name, tracker);
                }
            }
        }

        std::this_thread::sleep_for(poll_interval_);
    }
}

void TopicMonitor::refreshTopicList() {
    // Query graph APIs outside the mutex
    auto topic_types = node_->get_topic_names_and_types();

    struct TopicCounts {
        std::string type;
        size_t pub_count = 0;
        size_t sub_count = 0;
    };
    std::unordered_map<std::string, TopicCounts> fresh;
    fresh.reserve(topic_types.size());

    for (const auto& [name, types] : topic_types) {
        TopicCounts tc;
        if (!types.empty()) tc.type = types[0];
        tc.pub_count = node_->count_publishers(name);
        tc.sub_count = node_->count_subscribers(name);
        fresh[name] = std::move(tc);
    }

    // Now hold the lock briefly to swap in the data and prune stale topics
    std::lock_guard lock(mutex_);
    // Prune topics no longer in the graph (unless they are watched)
    for (auto it = topics_.begin(); it != topics_.end();) {
        if (fresh.find(it->first) == fresh.end() &&
            hz_trackers_.find(it->first) == hz_trackers_.end()) {
            it = topics_.erase(it);
        } else {
            ++it;
        }
    }

    for (const auto& [name, tc] : fresh) {
        auto& info = topics_[name];
        info.name = name;
        info.type = tc.type;
        info.publisher_count = static_cast<int>(tc.pub_count);
        info.subscriber_count = static_cast<int>(tc.sub_count);
        info.last_updated = std::chrono::steady_clock::now();
    }
}

double TopicMonitor::computeHz(const HzTracker& tracker) const {
    if (tracker.timestamps.size() < 2) return 0.0;

    // Only consider messages from the last 5 seconds
    // Compute running sum in-place without allocating a vector
    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - std::chrono::seconds(5);

    double interval_sum = 0.0;
    size_t interval_count = 0;

    for (size_t i = 1; i < tracker.timestamps.size(); ++i) {
        if (tracker.timestamps[i] < cutoff || tracker.timestamps[i - 1] < cutoff) continue;
        auto dt = std::chrono::duration<double>(
            tracker.timestamps[i] - tracker.timestamps[i - 1]);
        interval_sum += dt.count();
        ++interval_count;
    }

    if (interval_count == 0) return 0.0;

    double avg_interval = interval_sum / static_cast<double>(interval_count);
    return (avg_interval > 0) ? 1.0 / avg_interval : 0.0;
}

}  // namespace rtl
