#pragma once

#include "sim/types.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sim {

// Latency distribution parameters for sampling service times.
struct LatencyParams {
  enum class Dist { Lognormal, Gamma, Linear };

  Dist dist = Dist::Lognormal;
  double param1 = 0.0;  // lognormal: mu (log-mean), gamma: shape, linear: base_ms
  double param2 = 0.0;  // lognormal: sigma, gamma: scale, linear: coeff
  double tail_multiplier = 1.0;  // occasional heavy-tail (e.g., cache miss)
  double tail_prob = 0.0;        // probability of applying tail_multiplier
};

// Per-tier configuration: token bucket, concurrency, pricing, failure rate.
struct TierConfig {
  std::string provider;
  int tier_id = 0;
  double rate_per_sec = 10.0;      // token bucket refill rate
  double capacity = 100.0;         // token bucket capacity
  int concurrency_cap = 4;         // max in-flight attempts
  double price_per_call = 0.001;
  double p_fail = 0.02;            // Bernoulli transient failure probability
  int default_timeout_ms = 30'000;
  int default_max_retries = 3;
};

// Default latency params per node type (can be overridden per tier).
struct LatencyConfig {
  std::unordered_map<NodeType, LatencyParams> by_type;

  LatencyConfig();
  const LatencyParams& Get(NodeType t) const;
};

// Default provider tiers for embed and llm resource classes.
struct ProviderConfig {
  std::vector<TierConfig> tiers;
  LatencyConfig latency;

  ProviderConfig();
};

}  // namespace sim
