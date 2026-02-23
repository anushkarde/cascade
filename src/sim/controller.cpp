#include "sim/controller.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numeric>
#include <thread>

namespace sim {

Controller::Controller(const ControllerConfig& config)
    : config_(config),
      provider_config_(),
      provider_mgr_(std::make_unique<ProviderManager>(provider_config_)),
      rng_(std::make_unique<SeededRng>(config_.seed)),
      sampler_(std::make_unique<LatencySampler>(provider_config_.latency, rng_.get())) {
  SchedulerConfig sched_cfg;
  sched_cfg.policy = config_.policy;
  sched_cfg.disable_hedging = config_.disable_hedging;
  sched_cfg.disable_escalation = config_.disable_escalation;
  sched_cfg.disable_dag_priority = config_.disable_dag_priority;

  for (int i = 0; i < config_.workflows; ++i) {
    WorkloadParams wp;
    wp.pdfs = config_.pdfs;
    wp.subqueries_per_iter = config_.subqueries;
    wp.max_iters = config_.iters;
    wp.seed = config_.seed;
    auto wf = std::make_unique<Workflow>(static_cast<WorkflowId>(i + 1), wp, provider_config_);
    workflows_[wf->id()] = std::move(wf);
  }

  trace_file_.open(config_.out_dir + "/trace.json");
  trace_ = std::make_unique<TraceWriter>(trace_file_);

  scheduler_ = std::make_unique<Scheduler>(
      sched_cfg, provider_mgr_.get(), &latency_store_, &cpu_queue_, &io_queue_, trace_.get());

  for (const auto& tier_ptr : provider_mgr_->tiers()) {
    for (int t = 0; t < tier_ptr->concurrency_cap(); ++t) {
      tier_workers_.emplace_back(TierWorkerLoop, tier_ptr.get(), sampler_.get(), rng_.get(),
                                 &result_queue_, config_.time_scale, &shutdown_,
                                 config_.heavy_tail_prob, config_.heavy_tail_multiplier);
    }
  }

  const int local_workers = 4;
  for (int t = 0; t < local_workers; ++t) {
    local_workers_.emplace_back(LocalWorkerLoop, &cpu_queue_, ResourceClass::cpu, sampler_.get(),
                                rng_.get(), &result_queue_, provider_config_.latency,
                                config_.time_scale, &shutdown_,
                                config_.heavy_tail_prob, config_.heavy_tail_multiplier);
  }
  for (int t = 0; t < 2; ++t) {
    local_workers_.emplace_back(LocalWorkerLoop, &io_queue_, ResourceClass::io, sampler_.get(),
                                rng_.get(), &result_queue_, provider_config_.latency,
                                config_.time_scale, &shutdown_,
                                config_.heavy_tail_prob, config_.heavy_tail_multiplier);
  }
}

Controller::~Controller() {
  shutdown_.store(true);
  result_queue_.Shutdown();
  cpu_queue_.Shutdown();
  io_queue_.Shutdown();
  if (scheduler_thread_.joinable()) scheduler_thread_.join();
  if (monitor_thread_.joinable()) monitor_thread_.join();
  for (auto& t : tier_workers_) t.join();
  for (auto& t : local_workers_) t.join();
  for (auto& [k, v] : cancelled_flags_) delete v;
}

bool Controller::IsCriticalPath(NodeId nid, WorkflowId wf_id) {
  auto it = workflows_.find(wf_id);
  if (it == workflows_.end()) return false;
  const Node& n = it->second->node(nid);
  return n.type == NodeType::Plan || n.type == NodeType::Aggregate ||
         n.type == NodeType::DecideNext || n.type == NodeType::ExtractEvidence;
}

void Controller::LaunchHedge(Workflow* wf, NodeId nid, double now_ms) {
  Node& n = wf->node_mut(nid);
  if (n.preference_list.size() < 2) return;
  const ExecutionOption* next_opt = &n.preference_list[1];
  Tier* tier = provider_mgr_->GetTier(next_opt->provider, next_opt->tier_id);
  if (!tier || !tier->can_accept()) return;

  const std::uint64_t key =
      (static_cast<std::uint64_t>(wf->id()) << 32) | static_cast<std::uint64_t>(nid);
  auto* flag = new std::atomic<bool>(false);
  cancelled_flags_[key] = flag;

  QueuedAttempt attempt;
  attempt.node_id = nid;
  attempt.workflow_id = wf->id();
  attempt.node_type = n.type;
  attempt.provider = next_opt->provider;
  attempt.tier_id = next_opt->tier_id;
  attempt.tokens_needed = 1;
  attempt.timeout_ms = next_opt->timeout_ms;
  attempt.max_retries = next_opt->max_retries;
  attempt.latency_ctx.node_type = n.type;
  attempt.latency_ctx.token_length_est = n.output_size_est;
  attempt.attempt_id = next_attempt_id_.fetch_add(1);
  attempt.cancelled = flag;

  tier->Enqueue(std::move(attempt));
  if (trace_) trace_->Emit(TraceEvent::HedgeLaunched, now_ms, wf->id(), nid, "hedge");
}

void Controller::SchedulerLoop() {
  const auto start = std::chrono::steady_clock::now();
  while (!shutdown_.load()) {
    const auto now = std::chrono::steady_clock::now();
    const double now_ms =
        std::chrono::duration<double, std::milli>(now - start).count() * config_.time_scale;

    {
      std::lock_guard lock(workflows_mutex_);
      std::unordered_map<WorkflowId, Workflow*> wf_ptrs;
      for (auto& [id, wf] : workflows_) {
        if (wf && !wf->done()) wf_ptrs[id] = wf.get();
      }
      scheduler_->Dispatch(
          wf_ptrs, now_ms, workflow_cost_, workflow_start_ms_, next_attempt_id_, cancelled_flags_,
          [this](NodeId nid, WorkflowId wfid) { return IsCriticalPath(nid, wfid); },
          [this](WorkflowId wfid, NodeId nid, double dispatch_now_ms) {
            if (workflow_start_ms_[wfid] < 0) workflow_start_ms_[wfid] = dispatch_now_ms;
            const std::uint64_t key =
                (static_cast<std::uint64_t>(wfid) << 32) | static_cast<std::uint64_t>(nid);
            attempt_start_time_[key] = std::chrono::steady_clock::now();
          });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(config_.scheduler_interval_ms));
  }
}

void Controller::MonitorLoop() {
  const auto start = std::chrono::steady_clock::now();
  while (!shutdown_.load()) {
    const auto now = std::chrono::steady_clock::now();
    const double now_ms =
        std::chrono::duration<double, std::milli>(now - start).count() * config_.time_scale;

    if (!config_.disable_hedging && config_.policy == SchedulerPolicy::full) {
      std::lock_guard lock(workflows_mutex_);
      for (auto& [wf_id, wf] : workflows_) {
        if (!wf || wf->done()) continue;
        for (const auto& [nid, n] : wf->nodes()) {
          if (n.state != NodeState::Queued) continue;
          const std::uint64_t key =
              (static_cast<std::uint64_t>(wf_id) << 32) | static_cast<std::uint64_t>(nid);
          auto it = attempt_start_time_.find(key);
          if (it == attempt_start_time_.end()) continue;
          const double runtime_simulated_ms =
              std::chrono::duration<double, std::milli>(now - it->second).count() *
              static_cast<double>(config_.time_scale);
          const double est_p95 =
              latency_store_.GetP95(n.type, n.preference_list.empty() ? "" : n.preference_list[0].provider,
                                   n.preference_list.empty() ? 0 : n.preference_list[0].tier_id);
          const double stretch = est_p95 > 0 ? runtime_simulated_ms / est_p95 : 0;
          if (stretch > config_.straggler_stretch_threshold && IsCriticalPath(nid, wf_id)) {
            LaunchHedge(wf.get(), nid, now_ms);
            break;
          }
        }
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void Controller::ProcessResults() {
  AttemptResult res;
  while (result_queue_.TryPop(res)) {
    std::lock_guard lock(workflows_mutex_);
    auto wf_it = workflows_.find(res.workflow_id);
    if (wf_it == workflows_.end()) continue;
    Workflow* wf = wf_it->second.get();
    if (!wf || wf->done()) continue;

    Node& n = wf->node_mut(res.node_id);
    if (IsTerminal(n.state)) continue;

    latency_store_.Record(n.type, res.provider, res.tier_id, res.duration_ms);
    workflow_cost_[res.workflow_id] += res.cost;

    const std::uint64_t key =
        (static_cast<std::uint64_t>(res.workflow_id) << 32) | static_cast<std::uint64_t>(res.node_id);

    if (res.success) {
      auto it = cancelled_flags_.find(key);
      if (it != cancelled_flags_.end()) {
        it->second->store(true);
      }
      wf->MarkSucceeded(res.node_id);
      if (trace_) trace_->Emit(TraceEvent::AttemptFinish, res.duration_ms, res.workflow_id,
                               res.node_id, "ok");
    } else {
      if (res.error == "cancelled") {
        wf->Cancel(res.node_id);
        if (trace_) trace_->Emit(TraceEvent::AttemptCancel, res.duration_ms, res.workflow_id,
                                 res.node_id, "hedge_loser");
      } else {
        wf->MarkFailed(res.node_id);
        if (trace_) trace_->Emit(TraceEvent::AttemptFail, res.duration_ms, res.workflow_id,
                                res.node_id, res.error);
      }
    }

    attempt_start_time_.erase(key);
    if (wf->done()) {
      workflows_done_.fetch_add(1);
      const double start_ms = workflow_start_ms_[res.workflow_id];
      const double now_simulated_ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - run_start_)
              .count() *
          static_cast<double>(config_.time_scale);
      WorkflowMetrics m;
      m.workflow_id = res.workflow_id;
      m.makespan_ms = (start_ms >= 0) ? (now_simulated_ms - start_ms) : now_simulated_ms;
      m.cost = workflow_cost_[res.workflow_id];
      workflow_metrics_.push_back(m);
      if (trace_) trace_->Emit(TraceEvent::WorkflowDone, m.makespan_ms, res.workflow_id, 0, "");
    }
  }
}

void Controller::Run() {
  run_start_ = std::chrono::steady_clock::now();
  for (auto& [id, wf] : workflows_) {
    workflow_start_ms_[id] = -1;
    workflow_cost_[id] = 0;
    wf->RefreshRunnable();
  }

  scheduler_thread_ = std::thread(&Controller::SchedulerLoop, this);
  monitor_thread_ = std::thread(&Controller::MonitorLoop, this);

  while (workflows_done_.load() < config_.workflows) {
    ProcessResults();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  shutdown_.store(true);
  if (scheduler_thread_.joinable()) scheduler_thread_.join();
  if (monitor_thread_.joinable()) monitor_thread_.join();

  const auto end = std::chrono::steady_clock::now();
  (void)end;

  std::vector<double> makespans;
  std::vector<double> costs;
  for (const auto& m : workflow_metrics_) {
    makespans.push_back(m.makespan_ms);
    costs.push_back(m.cost);
  }
  std::sort(makespans.begin(), makespans.end());
  std::sort(costs.begin(), costs.end());
  const std::size_t n = makespans.size();
  if (n > 0) {
    summary_metrics_.makespan_mean_ms =
        std::accumulate(makespans.begin(), makespans.end(), 0.0) / static_cast<double>(n);
    summary_metrics_.makespan_p50_ms = makespans[n / 2];
    summary_metrics_.makespan_p95_ms = makespans[static_cast<std::size_t>(0.95 * n)];
    summary_metrics_.makespan_p99_ms = makespans[static_cast<std::size_t>(0.99 * n)];
    summary_metrics_.cost_mean =
        std::accumulate(costs.begin(), costs.end(), 0.0) / static_cast<double>(n);
    summary_metrics_.cost_p50 = costs[n / 2];
  }

  std::vector<TierStats> tier_stats;
  for (const auto& t : provider_mgr_->tiers()) {
    TierStats s;
    s.provider = t->provider();
    s.tier_id = t->tier_id();
    s.queue_wait_p95_ms = latency_store_.GetP95QueueWait(s.provider, s.tier_id);
    tier_stats.push_back(s);
  }
  WriteWorkflowsCsv(config_.out_dir, workflow_metrics_);
  WriteTiersCsv(config_.out_dir, tier_stats);
  WriteSummaryCsv(config_.out_dir, summary_metrics_);
}

}  // namespace sim
