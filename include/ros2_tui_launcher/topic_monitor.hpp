#pragma once

#include <rclcpp/rclcpp.hpp>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
#include <rigtorp/SPSCQueue.h>
#pragma GCC diagnostic pop

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rtl {

/// Information about a monitored topic.
struct TopicInfo {
    std::string name;
    std::string type;
    double hz = 0.0;             ///< Measured frequency
    double expected_hz = 0.0;    ///< Expected frequency (0 = no expectation)
    int publisher_count = 0;
    int subscriber_count = 0;
    bool stale = false;          ///< True if hz < expected_hz * 0.5
    std::chrono::steady_clock::time_point last_updated;
};

/// Monitors ROS 2 topics using rclcpp graph introspection.
/// For Hz measurement, creates generic subscriptions to watched topics
/// and measures message arrival rate.
class TopicMonitor {
public:
    /// @param node  rclcpp node used for subscriptions and graph queries
    /// @param poll_interval  How often to refresh topic list from graph
    explicit TopicMonitor(
        rclcpp::Node::SharedPtr node,
        std::chrono::milliseconds poll_interval = std::chrono::milliseconds(2000));
    ~TopicMonitor();

    TopicMonitor(const TopicMonitor&) = delete;
    TopicMonitor& operator=(const TopicMonitor&) = delete;

    /// Set topics to actively monitor for Hz. Creates subscriptions.
    /// @param topics  Vector of (topic_name, expected_hz)
    void setWatchedTopics(const std::vector<std::pair<std::string, double>>& topics);

    /// Start the monitoring background thread.
    void start();

    /// Stop the monitoring background thread.
    void stop();

    /// Get current snapshot of all topic info.
    std::vector<TopicInfo> snapshot() const;

    bool running() const { return running_.load(); }

private:
    struct HzTracker {
        /// Lock-free queue: subscription callback (producer) → poll thread (consumer)
        std::unique_ptr<rigtorp::SPSCQueue<std::chrono::steady_clock::time_point>> incoming;
        /// Drained timestamps owned by poll thread only — no lock needed
        std::deque<std::chrono::steady_clock::time_point> timestamps;
        rclcpp::GenericSubscription::SharedPtr subscription;
        double expected_hz = 0.0;
        static constexpr size_t kMaxSamples = 100;
        static constexpr size_t kQueueCapacity = 256;
    };

    void pollLoop();
    void refreshTopicList();
    double computeHz(const HzTracker& tracker) const;
    void tryCreateSubscription(const std::string& name, HzTracker& tracker);

    rclcpp::Node::SharedPtr node_;
    std::chrono::milliseconds poll_interval_;
    std::atomic<bool> running_{false};
    std::thread poll_thread_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, TopicInfo> topics_;
    std::unordered_map<std::string, HzTracker> hz_trackers_;
};

}  // namespace rtl
