#pragma once

#include "sim/config.h"
#include "sim/metrics.h"
#include "sim/provider.h"
#include "sim/scheduler.h"
#include "sim/trace.h"
#include "sim/types.h"
#include "sim/workflow.h"
#include "sim/worker.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

namespace sim {

struct ControllerConfig {
  int workflows = 100;
  int pdfs = 10;
  int iters = 3;
  int subqueries = 4;
  std::uint64_t seed = 1;
  int time_scale = 50;
  std::string out_dir = "out";
  SchedulerPolicy policy = SchedulerPolicy::full;
  bool disable_hedging = false;
  bool disable_escalation = false;
  bool disable_dag_priority = false;
  int scheduler_interval_ms = 50;
  double straggler_stretch_threshold = 1.5;
  double heavy_tail_prob = 0.02;      // 1-5% of tasks get heavy-tail multiplier
  double heavy_tail_multiplier = 50.0;  // multiplier for heavy-tail tasks
};

class Controller {
 public:
  explicit Controller(const ControllerConfig& config);
  ~Controller();

  void Run();
  const std::vector<WorkflowMetrics>& workflow_metrics() const { return workflow_metrics_; }
  const SummaryMetrics& summary_metrics() const { return summary_metrics_; }

 private:
  void SchedulerLoop();
  void MonitorLoop();
  void ProcessResults();
  bool IsCriticalPath(NodeId nid, WorkflowId wf_id);
  void LaunchHedge(Workflow* wf, NodeId nid, double now_ms);

  ControllerConfig config_;
  ProviderConfig provider_config_;
  std::unique_ptr<ProviderManager> provider_mgr_;
  LatencyEstimateStore latency_store_;
  ResultQueue result_queue_;
  LocalQueue cpu_queue_;
  LocalQueue io_queue_;

  std::unordered_map<WorkflowId, std::unique_ptr<Workflow>> workflows_;
  std::unordered_map<WorkflowId, double> workflow_start_ms_;
  std::unordered_map<WorkflowId, double> workflow_cost_;
  std::unordered_map<std::uint64_t, std::atomic<bool>*> cancelled_flags_;
  std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> attempt_start_time_;

  std::atomic<AttemptId> next_attempt_id_{1};
  std::atomic<bool> shutdown_{false};
  std::atomic<int> workflows_done_{0};

  std::unique_ptr<SeededRng> rng_;
  std::unique_ptr<LatencySampler> sampler_;
  std::unique_ptr<Scheduler> scheduler_;
  std::ofstream trace_file_;
  std::unique_ptr<TraceWriter> trace_;

  std::vector<std::thread> tier_workers_;
  std::vector<std::thread> local_workers_;
  std::thread scheduler_thread_;
  std::thread monitor_thread_;

  std::vector<WorkflowMetrics> workflow_metrics_;
  SummaryMetrics summary_metrics_;
  std::mutex workflows_mutex_;
  std::chrono::steady_clock::time_point run_start_;
};

}  // namespace sim
