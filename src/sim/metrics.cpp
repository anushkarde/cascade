#include "sim/metrics.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace sim {

QuantileEstimator::QuantileEstimator(std::size_t max_samples) : max_samples_(max_samples) {}

void QuantileEstimator::Add(double value) {
  samples_.push_back(value);
  if (samples_.size() > max_samples_) samples_.pop_front();
}

double QuantileEstimator::P50() const {
  if (samples_.empty()) return 100.0;
  sorted_.assign(samples_.begin(), samples_.end());
  std::sort(sorted_.begin(), sorted_.end());
  const std::size_t idx = static_cast<std::size_t>(0.50 * static_cast<double>(sorted_.size()));
  return sorted_[std::min(idx, sorted_.size() - 1)];
}

double QuantileEstimator::P90() const {
  if (samples_.empty()) return 200.0;
  sorted_.assign(samples_.begin(), samples_.end());
  std::sort(sorted_.begin(), sorted_.end());
  const std::size_t idx = static_cast<std::size_t>(0.90 * static_cast<double>(sorted_.size()));
  return sorted_[std::min(idx, sorted_.size() - 1)];
}

double QuantileEstimator::P95() const {
  if (samples_.empty()) return 300.0;
  sorted_.assign(samples_.begin(), samples_.end());
  std::sort(sorted_.begin(), sorted_.end());
  const std::size_t idx = static_cast<std::size_t>(0.95 * static_cast<double>(sorted_.size()));
  return sorted_[std::min(idx, sorted_.size() - 1)];
}

std::size_t LatencyEstKeyHash::operator()(const LatencyEstKey& k) const {
  std::size_t h = static_cast<std::size_t>(k.node_type) * 31;
  h ^= std::hash<std::string>{}(k.provider);
  h = h * 31 + static_cast<std::size_t>(k.tier_id);
  return h;
}

void LatencyEstimateStore::Record(NodeType type, const std::string& provider, int tier_id,
                                  double duration_ms) {
  std::lock_guard lock(mutex_);
  LatencyEstKey key{type, provider, tier_id};
  by_key_[key].Add(duration_ms);
}

double LatencyEstimateStore::GetP50(NodeType type, const std::string& provider, int tier_id) const {
  std::lock_guard lock(mutex_);
  LatencyEstKey key{type, provider, tier_id};
  auto it = by_key_.find(key);
  if (it == by_key_.end()) return 100.0;
  return it->second.P50();
}

double LatencyEstimateStore::GetP95(NodeType type, const std::string& provider, int tier_id) const {
  std::lock_guard lock(mutex_);
  LatencyEstKey key{type, provider, tier_id};
  auto it = by_key_.find(key);
  if (it == by_key_.end()) return 300.0;
  return it->second.P95();
}

void LatencyEstimateStore::RecordQueueWait(const std::string& provider, int tier_id,
                                          double wait_ms) {
  std::lock_guard lock(mutex_);
  queue_wait_[provider][tier_id].Add(wait_ms);
}

double LatencyEstimateStore::GetP95QueueWait(const std::string& provider, int tier_id) const {
  std::lock_guard lock(mutex_);
  auto pit = queue_wait_.find(provider);
  if (pit == queue_wait_.end()) return 50.0;
  auto tit = pit->second.find(tier_id);
  if (tit == pit->second.end()) return 50.0;
  return tit->second.P95();
}

static void WriteCsv(const std::string& path, const std::vector<std::string>& headers,
                     const std::vector<std::vector<std::string>>& rows) {
  std::ofstream out(path);
  if (!out) return;
  for (std::size_t i = 0; i < headers.size(); ++i) {
    if (i > 0) out << ",";
    out << headers[i];
  }
  out << "\n";
  for (const auto& row : rows) {
    for (std::size_t i = 0; i < row.size(); ++i) {
      if (i > 0) out << ",";
      out << row[i];
    }
    out << "\n";
  }
}

void WriteWorkflowsCsv(const std::string& out_dir, const std::vector<WorkflowMetrics>& metrics) {
  std::vector<std::string> headers = {"workflow_id", "makespan_ms", "cost", "retries",
                                     "cancellations", "hedges_launched", "wasted_ms"};
  std::vector<std::vector<std::string>> rows;
  for (const auto& m : metrics) {
    std::ostringstream ms, c, w;
    ms << m.makespan_ms;
    c << m.cost;
    w << m.wasted_ms;
    rows.push_back({std::to_string(m.workflow_id), ms.str(), c.str(), std::to_string(m.retries),
                    std::to_string(m.cancellations), std::to_string(m.hedges_launched), w.str()});
  }
  WriteCsv(out_dir + "/workflows.csv", headers, rows);
}

void WriteTiersCsv(const std::string& out_dir, const std::vector<TierStats>& stats) {
  std::vector<std::string> headers = {"provider", "tier_id", "utilization", "queue_wait_p95_ms",
                                      "in_flight_avg"};
  std::vector<std::vector<std::string>> rows;
  for (const auto& s : stats) {
    std::ostringstream u, q;
    u << s.utilization;
    q << s.queue_wait_p95_ms;
    rows.push_back({s.provider, std::to_string(s.tier_id), u.str(), q.str(),
                    std::to_string(s.in_flight_avg)});
  }
  WriteCsv(out_dir + "/tiers.csv", headers, rows);
}

void WriteSummaryCsv(const std::string& out_dir, const SummaryMetrics& summary) {
  std::vector<std::string> headers = {"makespan_mean_ms", "makespan_p50_ms", "makespan_p95_ms",
                                     "makespan_p99_ms", "cost_mean", "cost_p50"};
  std::ostringstream m1, m2, m3, m4, c1, c2;
  m1 << summary.makespan_mean_ms;
  m2 << summary.makespan_p50_ms;
  m3 << summary.makespan_p95_ms;
  m4 << summary.makespan_p99_ms;
  c1 << summary.cost_mean;
  c2 << summary.cost_p50;
  std::vector<std::vector<std::string>> rows = {
      {m1.str(), m2.str(), m3.str(), m4.str(), c1.str(), c2.str()}};
  WriteCsv(out_dir + "/summary.csv", headers, rows);
}

}  // namespace sim
