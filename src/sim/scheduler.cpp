#include "sim/scheduler.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace sim {

static const double kDefaultEstimateMs = 100.0;

Scheduler::Scheduler(const SchedulerConfig& config, ProviderManager* provider_mgr,
                    LatencyEstimateStore* latency_store, LocalQueue* cpu_queue,
                    LocalQueue* io_queue, TraceWriter* trace)
    : config_(config),
      provider_mgr_(provider_mgr),
      latency_store_(latency_store),
      cpu_queue_(cpu_queue),
      io_queue_(io_queue),
      trace_(trace) {}

double Scheduler::RemainingCriticalPath(const Workflow& wf, NodeId nid) const {
  const Node& n = wf.node(nid);
  double est = kDefaultEstimateMs;
  if (!n.preference_list.empty()) {
    const auto& opt = n.preference_list.front();
    est = latency_store_->GetP50(n.type, opt.provider, opt.tier_id);
  }
  if (n.children.empty()) return est;
  double max_child = 0.0;
  for (NodeId cid : n.children) {
    const Node& c = wf.node(cid);
    if (IsTerminal(c.state)) continue;
    double child_cp = RemainingCriticalPath(wf, cid);
    if (child_cp > max_child) max_child = child_cp;
  }
  return est + max_child;
}

std::vector<Scheduler::ScoredNode> Scheduler::ScoreAndSort(
    std::unordered_map<WorkflowId, Workflow*>& workflows, double now_ms,
    std::unordered_map<WorkflowId, double>& workflow_start_ms,
    std::function<bool(NodeId, WorkflowId)> is_critical_path) {
  (void)is_critical_path;
  std::vector<ScoredNode> scored;
  for (auto& [wf_id, wf] : workflows) {
    if (!wf || wf->done()) continue;
    const auto runnable = wf->RunnableNodes();
    const double start_ms = workflow_start_ms[wf_id];
    for (NodeId nid : runnable) {
      const Node& n = wf->node(nid);
      double score = 0.0;
      double age_ms = now_ms - start_ms;

      if (config_.disable_dag_priority ||
          config_.policy == SchedulerPolicy::fifo_cheapest) {
        score = age_ms;
      } else {
        const double rem_cp = RemainingCriticalPath(*wf, nid);
        double slack = 0.0;
        if (n.children.size() > 0) {
          double min_child_start = std::numeric_limits<double>::max();
          for (NodeId cid : n.children) {
            const Node& c = wf->node(cid);
            if (!IsActive(c.state)) continue;
            double child_cp = RemainingCriticalPath(*wf, cid);
            if (child_cp < min_child_start) min_child_start = child_cp;
          }
          if (min_child_start < std::numeric_limits<double>::max()) {
            double my_est = kDefaultEstimateMs;
            if (!n.preference_list.empty()) {
              const auto& opt = n.preference_list.front();
              my_est = latency_store_->GetP50(n.type, opt.provider, opt.tier_id);
            }
            slack = std::max(0.0, min_child_start - my_est);
          }
        }
        score = config_.alpha * rem_cp + config_.beta * (1.0 / (1.0 + slack)) +
                config_.gamma * age_ms;
      }
      scored.push_back({nid, wf_id, score, age_ms});
    }
  }
  std::sort(scored.begin(), scored.end(),
            [](const ScoredNode& a, const ScoredNode& b) { return a.score > b.score; });
  return scored;
}

const ExecutionOption* Scheduler::SelectOption(
    const Node& n, std::unordered_map<WorkflowId, double>& workflow_cost, bool is_critical) {
  if (n.preference_list.empty()) return nullptr;
  const double budget_left = config_.budget_per_workflow - workflow_cost[n.workflow_id];
  const ExecutionOption* chosen = nullptr;
  for (const auto& opt : n.preference_list) {
    if (opt.price_per_call > budget_left) continue;
    Tier* t = provider_mgr_->GetTier(opt.provider, opt.tier_id);
    if (!t || !t->can_accept()) continue;
    chosen = &opt;
    if (config_.disable_escalation || config_.policy == SchedulerPolicy::fifo_cheapest ||
        config_.policy == SchedulerPolicy::dag_cheapest) {
      break;
    }
    if (!is_critical) break;
    const double delta_cost = opt.price_per_call - n.preference_list.front().price_per_call;
    if (delta_cost <= 0) break;
    const double ect_cheap =
        latency_store_->GetP95QueueWait(n.preference_list.front().provider,
                                       n.preference_list.front().tier_id) +
        latency_store_->GetP50(n.type, n.preference_list.front().provider,
                              n.preference_list.front().tier_id);
    const double ect_fast =
        latency_store_->GetP95QueueWait(opt.provider, opt.tier_id) +
        latency_store_->GetP50(n.type, opt.provider, opt.tier_id);
    const double benefit = ect_cheap - ect_fast;
    if (benefit / delta_cost >= config_.escalation_benefit_cost_threshold) {
      chosen = &opt;
    }
    break;
  }
  return chosen ? chosen : (n.preference_list.empty() ? nullptr : &n.preference_list.front());
}

int Scheduler::Dispatch(std::unordered_map<WorkflowId, Workflow*>& workflows, double now_ms,
                         std::unordered_map<WorkflowId, double>& workflow_cost,
                         std::unordered_map<WorkflowId, double>& workflow_start_ms,
                         std::atomic<AttemptId>& next_attempt_id,
                         std::unordered_map<std::uint64_t, std::atomic<bool>*>& cancelled_flags,
                         std::function<bool(NodeId, WorkflowId)> is_critical_path,
                         std::function<void(WorkflowId, NodeId, double)> on_dispatch) {
  auto scored = ScoreAndSort(workflows, now_ms, workflow_start_ms, is_critical_path);

  int in_flight = 0;
  for (auto& [wf_id, wf] : workflows) {
    if (!wf || wf->done()) continue;
    for (const auto& [nid, n] : wf->nodes()) {
      if (n.state == NodeState::Queued || n.state == NodeState::Running) ++in_flight;
    }
  }

  int dispatched = 0;
  for (const auto& sn : scored) {
    if (in_flight >= config_.max_in_flight_global) break;

    Workflow* wf = workflows[sn.workflow_id];
    if (!wf || wf->done()) continue;

    Node& n = wf->node_mut(sn.node_id);
    if (n.state != NodeState::Runnable) continue;

    if (n.resource_class == ResourceClass::cpu || n.resource_class == ResourceClass::io) {
      LocalTask task;
      task.node_id = sn.node_id;
      task.workflow_id = sn.workflow_id;
      task.node_type = n.type;
      task.resource_class = n.resource_class;
      task.latency_ctx.node_type = n.type;
      task.latency_ctx.pdf_size_est = n.output_size_est;
      task.latency_ctx.num_chunks_est = 50;
      task.timeout_ms = 5000;
      task.attempt_id = next_attempt_id.fetch_add(1);

      wf->MarkQueued(sn.node_id);
      if (n.resource_class == ResourceClass::cpu)
        cpu_queue_->Push(std::move(task));
      else
        io_queue_->Push(std::move(task));
      if (trace_) trace_->Emit(TraceEvent::NodeQueued, now_ms, sn.workflow_id, sn.node_id, "local");
      if (on_dispatch) on_dispatch(sn.workflow_id, sn.node_id, now_ms);
      ++dispatched;
      ++in_flight;
      continue;
    }

    std::string dispatch_provider;
    int dispatch_tier_id = 0;
    int dispatch_timeout_ms = 30'000;
    int dispatch_max_retries = 3;
    Tier* tier = nullptr;

    if (config_.enable_model_routing && !n.preference_list.empty()) {
      const bool is_critical = is_critical_path ? is_critical_path(sn.node_id, sn.workflow_id) : false;
      const ExecutionOption* opt = SelectOption(n, workflow_cost, is_critical);
      if (!opt) continue;
      tier = provider_mgr_->GetTier(opt->provider, opt->tier_id);
      if (!tier || !tier->can_accept()) continue;
      dispatch_provider = opt->provider;
      dispatch_tier_id = opt->tier_id;
      dispatch_timeout_ms = opt->timeout_ms;
      dispatch_max_retries = opt->max_retries;
    } else {
      const char* provider_name =
          (n.resource_class == ResourceClass::embed) ? "embed_provider" : "llm_provider";
      for (const auto& t : provider_mgr_->tiers()) {
        if (t->provider() == provider_name && t->can_accept()) {
          tier = t.get();
          break;
        }
      }
      if (!tier) continue;
      dispatch_provider = tier->provider();
      dispatch_tier_id = tier->tier_id();
      dispatch_timeout_ms = tier->config().default_timeout_ms;
      dispatch_max_retries = tier->config().default_max_retries;
    }

    const std::uint64_t key =
        (static_cast<std::uint64_t>(sn.workflow_id) << 32) | static_cast<std::uint64_t>(sn.node_id);
    auto* flag = new std::atomic<bool>(false);
    cancelled_flags[key] = flag;

    QueuedAttempt attempt;
    attempt.node_id = sn.node_id;
    attempt.workflow_id = sn.workflow_id;
    attempt.node_type = n.type;
    attempt.provider = dispatch_provider;
    attempt.tier_id = dispatch_tier_id;
    attempt.tokens_needed = 1;
    attempt.timeout_ms = dispatch_timeout_ms;
    attempt.max_retries = dispatch_max_retries;
    attempt.latency_ctx.node_type = n.type;
    attempt.latency_ctx.token_length_est = static_cast<std::size_t>(n.output_size_est);
    attempt.attempt_id = next_attempt_id.fetch_add(1);
    attempt.cancelled = flag;

    wf->MarkQueued(sn.node_id);
    tier->Enqueue(std::move(attempt));
    if (trace_) trace_->Emit(TraceEvent::NodeQueued, now_ms, sn.workflow_id, sn.node_id,
                            dispatch_provider + "_" + std::to_string(dispatch_tier_id));
    if (on_dispatch) on_dispatch(sn.workflow_id, sn.node_id, now_ms);
    ++dispatched;
    ++in_flight;
  }
  return dispatched;
}

}  // namespace sim
