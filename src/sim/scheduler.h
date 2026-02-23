#pragma once

#include "sim/config.h"
#include "sim/metrics.h"
#include "sim/provider.h"
#include "sim/trace.h"
#include "sim/types.h"
#include "sim/workflow.h"
#include "sim/worker.h"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace sim {

enum class SchedulerPolicy {
  fifo_cheapest,
  dag_cheapest,
  dag_escalation,
  full,
};

struct SchedulerConfig {
  SchedulerPolicy policy = SchedulerPolicy::full;
  bool disable_hedging = false;
  bool disable_escalation = false;
  bool disable_dag_priority = false;
  int max_in_flight_global = 200;
  double budget_per_workflow = 10.0;
  double escalation_benefit_cost_threshold = 0.5;
  double alpha = 1.0;   // remCP weight
  double beta = 0.5;     // slack weight
  double gamma = 0.1;   // age weight
};

// Dispatches runnable nodes to provider/local queues.
// Caller provides workflows, provider manager, local queues, and shared state.
class Scheduler {
 public:
  Scheduler(const SchedulerConfig& config, ProviderManager* provider_mgr,
            LatencyEstimateStore* latency_store, LocalQueue* cpu_queue, LocalQueue* io_queue,
            TraceWriter* trace);

  // Dispatch pass: score runnable nodes, select options, enqueue.
  // workflows: mutable refs; caller holds lock.
  // cancelled_flags: key = (wf_id << 32) | node_id for unique identification.
  // on_dispatch: optional callback (wf_id, node_id, now_ms) when a task is enqueued.
  // Returns number of nodes dispatched.
  int Dispatch(std::unordered_map<WorkflowId, Workflow*>& workflows, double now_ms,
               std::unordered_map<WorkflowId, double>& workflow_cost,
               std::unordered_map<WorkflowId, double>& workflow_start_ms,
               std::atomic<AttemptId>& next_attempt_id,
               std::unordered_map<std::uint64_t, std::atomic<bool>*>& cancelled_flags,
               std::function<bool(NodeId, WorkflowId)> is_critical_path,
               std::function<void(WorkflowId, NodeId, double)> on_dispatch = nullptr);

 private:
  struct ScoredNode {
    NodeId node_id;
    WorkflowId workflow_id;
    double score;
    double age_ms;
  };

  std::vector<ScoredNode> ScoreAndSort(
      std::unordered_map<WorkflowId, Workflow*>& workflows, double now_ms,
      std::unordered_map<WorkflowId, double>& workflow_start_ms,
      std::function<bool(NodeId, WorkflowId)> is_critical_path);

  const ExecutionOption* SelectOption(const Node& n,
                                      std::unordered_map<WorkflowId, double>& workflow_cost,
                                      bool is_critical);

  double RemainingCriticalPath(const Workflow& wf, NodeId nid) const;

  SchedulerConfig config_;
  ProviderManager* provider_mgr_;
  LatencyEstimateStore* latency_store_;
  LocalQueue* cpu_queue_;
  LocalQueue* io_queue_;
  TraceWriter* trace_;
};

}  // namespace sim
