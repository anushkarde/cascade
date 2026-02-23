#pragma once

#include "sim/types.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sim {

// Rolling quantile estimator: keeps recent samples, computes approximate p50/p90/p95.
class QuantileEstimator {
 public:
  explicit QuantileEstimator(std::size_t max_samples = 1000);

  void Add(double value);
  double P50() const;
  double P90() const;
  double P95() const;
  std::size_t Count() const { return samples_.size(); }

 private:
  std::deque<double> samples_;
  std::size_t max_samples_;
  mutable std::vector<double> sorted_;
};

// Key for (node_type, provider, tier_id) latency estimates.
struct LatencyEstKey {
  NodeType node_type;
  std::string provider;
  int tier_id = 0;

  bool operator==(const LatencyEstKey& o) const {
    return node_type == o.node_type && provider == o.provider && tier_id == o.tier_id;
  }
};

struct LatencyEstKeyHash {
  std::size_t operator()(const LatencyEstKey& k) const;
};

// Thread-safe store of latency estimates per (type, tier).
class LatencyEstimateStore {
 public:
  void Record(NodeType type, const std::string& provider, int tier_id, double duration_ms);
  double GetP50(NodeType type, const std::string& provider, int tier_id) const;
  double GetP95(NodeType type, const std::string& provider, int tier_id) const;
  double GetP95QueueWait(const std::string& provider, int tier_id) const;
  void RecordQueueWait(const std::string& provider, int tier_id, double wait_ms);

 private:
  mutable std::mutex mutex_;
  std::unordered_map<LatencyEstKey, QuantileEstimator, LatencyEstKeyHash> by_key_;
  std::unordered_map<std::string, std::unordered_map<int, QuantileEstimator>> queue_wait_;
};

// Per-workflow metrics for CSV output.
struct WorkflowMetrics {
  WorkflowId workflow_id = 0;
  double makespan_ms = 0.0;
  double cost = 0.0;
  int retries = 0;
  int cancellations = 0;
  int hedges_launched = 0;
  double wasted_ms = 0.0;
};

// Per-tier stats over a time window.
struct TierStats {
  std::string provider;
  int tier_id = 0;
  double utilization = 0.0;
  double queue_wait_p95_ms = 0.0;
  int in_flight_avg = 0;
};

// Aggregate summary.
struct SummaryMetrics {
  double makespan_mean_ms = 0.0;
  double makespan_p50_ms = 0.0;
  double makespan_p95_ms = 0.0;
  double makespan_p99_ms = 0.0;
  double cost_mean = 0.0;
  double cost_p50 = 0.0;
};

// Writes CSV files to out_dir.
void WriteWorkflowsCsv(const std::string& out_dir, const std::vector<WorkflowMetrics>& metrics);
void WriteTiersCsv(const std::string& out_dir, const std::vector<TierStats>& stats);
void WriteSummaryCsv(const std::string& out_dir, const SummaryMetrics& summary);

}  // namespace sim
