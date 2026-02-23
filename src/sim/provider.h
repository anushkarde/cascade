#pragma once

#include "sim/config.h"
#include "sim/random.h"
#include "sim/types.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sim {

// Context passed to latency sampling for node-type-specific formulas.
struct LatencyContext {
  NodeType node_type = NodeType::Plan;
  std::size_t pdf_size_est = 0;
  int num_chunks_est = 0;
  std::size_t token_length_est = 100;
};

// Result of sampling: service time in ms, and whether a transient failure occurred.
struct LatencySample {
  double service_time_ms = 0.0;
  bool failed = false;
  bool timeout = false;
};

// Thread-safe token bucket: rate r tokens/sec, capacity B.
// Acquire(tokens) blocks until tokens are available.
class TokenBucket {
 public:
  TokenBucket(double rate_per_sec, double capacity);

  void Acquire(double tokens);

 private:
  void Refill();

  double rate_per_sec_;
  double capacity_;
  double tokens_;
  std::chrono::steady_clock::time_point last_refill_;
  std::mutex mutex_;
  std::condition_variable cv_;
};

// Work item enqueued to a tier.
struct QueuedAttempt {
  NodeId node_id = 0;
  WorkflowId workflow_id = 0;
  NodeType node_type = NodeType::Plan;
  std::string provider;
  int tier_id = 0;
  int tokens_needed = 1;
  int timeout_ms = 30'000;
  int max_retries = 3;
  LatencyContext latency_ctx;
};

// Single provider tier: queue, token bucket, concurrency cap.
class Tier {
 public:
  Tier(const TierConfig& config);

  const TierConfig& config() const { return config_; }
  const std::string& provider() const { return config_.provider; }
  int tier_id() const { return config_.tier_id; }
  int concurrency_cap() const { return config_.concurrency_cap; }
  int in_flight() const { return in_flight_.load(); }
  bool can_accept() const { return in_flight_.load() < config_.concurrency_cap; }

  void Enqueue(QueuedAttempt attempt);
  bool TryDequeue(QueuedAttempt& out);
  void BlockingDequeue(QueuedAttempt& out);
  void AcquireTokens(QueuedAttempt& attempt);
  void OnAttemptStart();
  void OnAttemptFinish();

  TokenBucket& token_bucket() { return *token_bucket_; }

 private:
  TierConfig config_;
  std::unique_ptr<TokenBucket> token_bucket_;
  std::queue<QueuedAttempt> queue_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::atomic<int> in_flight_{0};
};

// Samples latency and failure for attempts using config and seeded RNG.
class LatencySampler {
 public:
  LatencySampler(const LatencyConfig& config, SeededRng* rng);

  LatencySample Sample(const LatencyContext& ctx, int timeout_ms, double p_fail);

 private:
  double SampleServiceTime(const LatencyParams& params, const LatencyContext& ctx);

  const LatencyConfig& config_;
  SeededRng* rng_;
};

// Manages all provider tiers.
class ProviderManager {
 public:
  explicit ProviderManager(const ProviderConfig& config);

  Tier* GetTier(const std::string& provider, int tier_id);
  const Tier* GetTier(const std::string& provider, int tier_id) const;
  const std::vector<std::unique_ptr<Tier>>& tiers() const { return tiers_; }

 private:
  std::vector<std::unique_ptr<Tier>> tiers_;
  std::unordered_map<std::string, std::unordered_map<int, std::size_t>> tier_index_;
};

}  // namespace sim
