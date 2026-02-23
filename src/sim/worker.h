#pragma once

#include "sim/config.h"
#include "sim/provider.h"
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

namespace sim {

struct AttemptResult {
  NodeId node_id = 0;
  WorkflowId workflow_id = 0;
  AttemptId attempt_id = 0;
  bool success = false;
  double duration_ms = 0.0;
  double cost = 0.0;
  std::string provider;
  int tier_id = 0;
  std::string error;  // "timeout", "failed", "cancelled"
};

// Thread-safe queue for results from workers to controller.
class ResultQueue {
 public:
  void Push(AttemptResult r);
  bool TryPop(AttemptResult& out);
  void BlockingPop(AttemptResult& out);
  void Shutdown();
  bool IsShutdown() const { return shutdown_.load(); }

 private:
  std::queue<AttemptResult> queue_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::atomic<bool> shutdown_{false};
};

// Local task for cpu/io resource classes (no provider tier).
struct LocalTask {
  NodeId node_id = 0;
  WorkflowId workflow_id = 0;
  NodeType node_type = NodeType::Plan;
  ResourceClass resource_class = ResourceClass::cpu;
  LatencyContext latency_ctx;
  int timeout_ms = 5000;
  AttemptId attempt_id = 0;
  std::atomic<bool>* cancelled = nullptr;
};

// Thread-safe local task queue.
class LocalQueue {
 public:
  void Push(LocalTask t);
  bool TryPop(LocalTask& out);
  void BlockingPop(LocalTask& out);
  bool TimedPop(LocalTask& out, std::chrono::milliseconds timeout);
  void Shutdown();
  bool IsShutdown() const { return shutdown_.load(); }

 private:
  std::queue<LocalTask> queue_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::atomic<bool> shutdown_{false};
};

// Runs worker loop for a provider tier. Exits when shutdown is true.
// heavy_tail_prob: if > 0, this fraction of tasks get service_time *= heavy_tail_multiplier.
void TierWorkerLoop(Tier* tier, LatencySampler* sampler, SeededRng* rng, ResultQueue* results,
                    int time_scale, std::atomic<bool>* shutdown,
                    double heavy_tail_prob = 0.0, double heavy_tail_multiplier = 1.0);

// Runs worker loop for local cpu or io queue.
void LocalWorkerLoop(LocalQueue* queue, ResourceClass resource_class, LatencySampler* sampler,
                    SeededRng* rng, ResultQueue* results, const LatencyConfig& latency_config,
                    int time_scale, std::atomic<bool>* shutdown,
                    double heavy_tail_prob = 0.0, double heavy_tail_multiplier = 1.0);

// Sleep in chunks, checking cancellation flag. Returns true if cancelled.
bool CancellableSleep(std::chrono::milliseconds total, std::atomic<bool>* cancelled,
                      int chunk_ms = 20);

}  // namespace sim
