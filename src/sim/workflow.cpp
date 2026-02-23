#include "sim/workflow.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace sim {

static std::uint64_t Mix64(std::uint64_t x) {
  // SplitMix64-ish finalizer (deterministic, fast).
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

Workflow::Workflow(WorkflowId id, WorkloadParams params, const ProviderConfig& provider_config)
    : id_(id), params_(params), provider_config_(&provider_config) {
  if (params_.pdfs <= 0) throw std::runtime_error("WorkloadParams.pdfs must be > 0");
  if (params_.subqueries_per_iter < 0) throw std::runtime_error("WorkloadParams.subqueries_per_iter must be >= 0");
  if (params_.max_iters <= 0) throw std::runtime_error("WorkloadParams.max_iters must be > 0");
  EnsureInitialPlan();
  RefreshRunnable();
}

const Node& Workflow::node(NodeId nid) const {
  auto it = graph_.nodes.find(nid);
  if (it == graph_.nodes.end()) throw std::runtime_error("Unknown node id");
  return it->second;
}

Node& Workflow::node_mut(NodeId nid) {
  auto it = graph_.nodes.find(nid);
  if (it == graph_.nodes.end()) throw std::runtime_error("Unknown node id");
  return it->second;
}

NodeId Workflow::NewNodeId() {
  return graph_.next_node_id++;
}

Node& Workflow::AddNode(Node n) {
  if (n.id == 0) n.id = NewNodeId();
  if (n.workflow_id == 0) n.workflow_id = id_;
  auto [it, inserted] = graph_.nodes.emplace(n.id, std::move(n));
  if (!inserted) throw std::runtime_error("Duplicate node id");
  return it->second;
}

void Workflow::AddEdge(NodeId from, NodeId to) {
  Node& a = node_mut(from);
  Node& b = node_mut(to);
  a.children.push_back(to);
  b.deps.push_back(from);
}

bool Workflow::DepsSatisfied(const Node& n) const {
  for (NodeId d : n.deps) {
    const Node& dep = node(d);
    if (dep.state != NodeState::Succeeded) return false;
  }
  return true;
}

void Workflow::InitializeStateFromDeps(NodeId nid) {
  Node& n = node_mut(nid);
  if (IsTerminal(n.state)) return;
  n.state = DepsSatisfied(n) ? NodeState::Runnable : NodeState::WaitingDeps;
}

void Workflow::SetState(NodeId nid, NodeState next) {
  Node& n = node_mut(nid);
  if (n.state == next) return;

  // Enforce invariants that matter for correctness, while remaining usable for early bring-up.
  auto require = [&](bool ok, const char* msg) {
    if (!ok) throw std::runtime_error(std::string("Invalid node transition: ") + msg);
  };

  if (IsTerminal(n.state)) {
    require(false, "terminal state cannot transition");
  }

  switch (next) {
    case NodeState::WaitingDeps:
      require(!DepsSatisfied(n), "cannot move to WaitingDeps when deps satisfied");
      break;
    case NodeState::Runnable:
      require(DepsSatisfied(n), "cannot move to Runnable before deps satisfied");
      break;
    case NodeState::Queued:
      require(n.state == NodeState::Runnable, "Queued only allowed from Runnable");
      break;
    case NodeState::Running:
      require(n.state == NodeState::Queued || n.state == NodeState::Runnable, "Running only allowed from Queued/Runnable");
      break;
    case NodeState::Succeeded:
      require(n.state == NodeState::Running || n.state == NodeState::Queued || n.state == NodeState::Runnable,
              "Succeeded only allowed from Running/Queued/Runnable");
      break;
    case NodeState::Failed:
      require(n.state == NodeState::Running || n.state == NodeState::Queued || n.state == NodeState::Runnable,
              "Failed only allowed from Running/Queued/Runnable");
      break;
    case NodeState::Cancelled:
      // Best-effort cancellation: allow from any non-terminal.
      break;
  }

  n.state = next;
}

std::vector<NodeId> Workflow::RefreshRunnable() {
  std::vector<NodeId> newly;
  newly.reserve(graph_.nodes.size());

  for (auto& [nid, n] : graph_.nodes) {
    if (IsTerminal(n.state) || n.state == NodeState::Queued || n.state == NodeState::Running) continue;
    const bool ready = DepsSatisfied(n);
    if (ready && n.state != NodeState::Runnable) {
      n.state = NodeState::Runnable;
      newly.push_back(nid);
    } else if (!ready && n.state != NodeState::WaitingDeps) {
      n.state = NodeState::WaitingDeps;
    }
  }

  return newly;
}

std::vector<NodeId> Workflow::RunnableNodes() const {
  std::vector<NodeId> out;
  out.reserve(graph_.nodes.size());
  for (const auto& [nid, n] : graph_.nodes) {
    if (n.state == NodeState::Runnable) out.push_back(nid);
  }
  std::sort(out.begin(), out.end());
  return out;
}

void Workflow::MarkQueued(NodeId nid) {
  SetState(nid, NodeState::Queued);
}

void Workflow::MarkRunning(NodeId nid) {
  SetState(nid, NodeState::Running);
}

void Workflow::MarkSucceeded(NodeId nid) {
  const NodeType t = node(nid).type;
  const int iter = node(nid).iter;

  SetState(nid, NodeState::Succeeded);

  if (t == NodeType::Plan) {
    ExpandIterationFromPlan(nid);
  } else if (t == NodeType::DecideNext) {
    OnDecideNext(nid);
    completed_iters_ = std::max(completed_iters_, iter + 1);
  }

  RefreshRunnable();
}

void Workflow::MarkFailed(NodeId nid) {
  SetState(nid, NodeState::Failed);
  RefreshRunnable();
}

void Workflow::Cancel(NodeId nid) {
  Node& n = node_mut(nid);
  if (IsTerminal(n.state)) return;
  n.state = NodeState::Cancelled;
  RefreshRunnable();
}

void Workflow::PruneAfterStop(int stop_iter) {
  for (auto& [nid, n] : graph_.nodes) {
    if (IsTerminal(n.state)) continue;
    if (n.iter > stop_iter) n.state = NodeState::Cancelled;
  }
  RefreshRunnable();
}

void Workflow::EnsureInitialPlan() {
  Node plan;
  plan.id = NewNodeId();
  plan.workflow_id = id_;
  plan.type = NodeType::Plan;
  plan.resource_class = ResourceClass::llm;
  plan.idempotent = true;
  plan.iter = 0;
  plan.state = NodeState::Runnable;  // root node has no deps.

  // Deterministic "size" estimate: grows slightly with iteration and workload.
  plan.output_size_est = static_cast<std::size_t>(200 + 10 * params_.subqueries_per_iter + 3 * params_.pdfs);

  Node& p = AddNode(std::move(plan));
  PopulatePreferenceListForNode(p);
}

static ResourceClass ResourceForType(NodeType t) {
  switch (t) {
    case NodeType::LoadPDF: return ResourceClass::io;
    case NodeType::Chunk: return ResourceClass::cpu;
    case NodeType::Embed: return ResourceClass::embed;
    case NodeType::SimilaritySearch: return ResourceClass::cpu;
    case NodeType::ExtractEvidence: return ResourceClass::llm;
    case NodeType::Plan: return ResourceClass::llm;
    case NodeType::Aggregate: return ResourceClass::cpu;
    case NodeType::DecideNext: return ResourceClass::llm;
  }
  return ResourceClass::cpu;
}

void Workflow::ExpandIterationFromPlan(NodeId plan_node) {
  const Node& plan = node(plan_node);
  const int iter = plan.iter;
  if (iter >= params_.max_iters) return;

  // Guard against double-expansion (e.g., if controller replays success).
  for (const auto& [nid, n] : graph_.nodes) {
    if (n.type == NodeType::Aggregate && n.iter == iter) return;
  }

  std::vector<NodeId> extract_nodes;
  extract_nodes.reserve(static_cast<std::size_t>(params_.pdfs) *
                        static_cast<std::size_t>(std::max(1, params_.subqueries_per_iter)));

  for (int p = 0; p < params_.pdfs; ++p) {
    Node load;
    load.id = NewNodeId();
    load.workflow_id = id_;
    load.type = NodeType::LoadPDF;
    load.resource_class = ResourceForType(load.type);
    load.idempotent = true;
    load.iter = iter;
    load.pdf_idx = p;

    Node chunk;
    chunk.id = NewNodeId();
    chunk.workflow_id = id_;
    chunk.type = NodeType::Chunk;
    chunk.resource_class = ResourceForType(chunk.type);
    chunk.idempotent = true;
    chunk.iter = iter;
    chunk.pdf_idx = p;

    Node embed;
    embed.id = NewNodeId();
    embed.workflow_id = id_;
    embed.type = NodeType::Embed;
    embed.resource_class = ResourceForType(embed.type);
    embed.idempotent = true;
    embed.iter = iter;
    embed.pdf_idx = p;

    const NodeId load_id = load.id;
    const NodeId chunk_id = chunk.id;
    const NodeId embed_id = embed.id;

    Node& l = AddNode(std::move(load));
    Node& c = AddNode(std::move(chunk));
    Node& e = AddNode(std::move(embed));
    PopulatePreferenceListForNode(l);
    PopulatePreferenceListForNode(c);
    PopulatePreferenceListForNode(e);

    AddEdge(plan_node, load_id);
    AddEdge(load_id, chunk_id);
    AddEdge(chunk_id, embed_id);

    const int K = params_.subqueries_per_iter;
    if (K == 0) continue;

    for (int q = 0; q < K; ++q) {
      Node ss;
      ss.id = NewNodeId();
      ss.workflow_id = id_;
      ss.type = NodeType::SimilaritySearch;
      ss.resource_class = ResourceForType(ss.type);
      ss.idempotent = true;
      ss.iter = iter;
      ss.pdf_idx = p;
      ss.subquery_idx = q;

      Node ex;
      ex.id = NewNodeId();
      ex.workflow_id = id_;
      ex.type = NodeType::ExtractEvidence;
      ex.resource_class = ResourceForType(ex.type);
      ex.idempotent = true;
      ex.iter = iter;
      ex.pdf_idx = p;
      ex.subquery_idx = q;

      // Deterministic evidence estimate: drives DecideNext without needing provider results.
      const std::uint64_t h = Mix64(params_.seed ^ (static_cast<std::uint64_t>(id_) << 32) ^
                                    (static_cast<std::uint64_t>(iter) * 0x9e3779b97f4a7c15ULL) ^
                                    (static_cast<std::uint64_t>(p) << 8) ^ static_cast<std::uint64_t>(q));
      ex.evidence_count_est = static_cast<int>(h % 4ULL);  // 0..3

      const NodeId ss_id = ss.id;
      const NodeId ex_id = ex.id;
      Node& ss_ref = AddNode(std::move(ss));
      Node& ex_ref = AddNode(std::move(ex));
      PopulatePreferenceListForNode(ss_ref);
      PopulatePreferenceListForNode(ex_ref);

      AddEdge(embed_id, ss_id);
      AddEdge(ss_id, ex_id);
      extract_nodes.push_back(ex_id);
    }
  }

  Node agg;
  agg.id = NewNodeId();
  agg.workflow_id = id_;
  agg.type = NodeType::Aggregate;
  agg.resource_class = ResourceForType(agg.type);
  agg.idempotent = true;
  agg.iter = iter;

  Node decide;
  decide.id = NewNodeId();
  decide.workflow_id = id_;
  decide.type = NodeType::DecideNext;
  decide.resource_class = ResourceForType(decide.type);
  decide.idempotent = true;
  decide.iter = iter;

  const NodeId agg_id = agg.id;
  const NodeId decide_id = decide.id;
  Node& agg_ref = AddNode(std::move(agg));
  Node& decide_ref = AddNode(std::move(decide));
  PopulatePreferenceListForNode(agg_ref);
  PopulatePreferenceListForNode(decide_ref);

  if (!extract_nodes.empty()) {
    for (NodeId ex_id : extract_nodes) AddEdge(ex_id, agg_id);
  } else {
    // If there are no subqueries, treat the iteration as producing no evidence but still progress.
    AddEdge(plan_node, agg_id);
  }
  AddEdge(agg_id, decide_id);

  InitializeStateFromDeps(agg_id);
  InitializeStateFromDeps(decide_id);
}

int Workflow::IterEvidenceTotal(int iter) const {
  int total = 0;
  for (const auto& [nid, n] : graph_.nodes) {
    if (n.iter == iter && n.type == NodeType::ExtractEvidence) total += n.evidence_count_est;
  }
  return total;
}

int Workflow::IterPdfCoverageCount(int iter) const {
  std::unordered_set<int> covered;
  covered.reserve(static_cast<std::size_t>(params_.pdfs));
  for (const auto& [nid, n] : graph_.nodes) {
    if (n.iter != iter || n.type != NodeType::ExtractEvidence) continue;
    if (n.evidence_count_est > 0) covered.insert(n.pdf_idx);
  }
  return static_cast<int>(covered.size());
}

DecideAction Workflow::ComputeDecideAction(int iter) const {
  if (iter + 1 >= params_.max_iters) return DecideAction::Stop;

  const int total = IterEvidenceTotal(iter);
  const int covered = IterPdfCoverageCount(iter);

  const double coverage = static_cast<double>(covered) / static_cast<double>(std::max(1, params_.pdfs));
  const double denom = static_cast<double>(std::max(1, params_.pdfs * std::max(1, params_.subqueries_per_iter) * 2));
  const double confidence = std::min(1.0, static_cast<double>(total) / denom);

  // Deterministic tie-breaker for borderline cases.
  const std::uint64_t h = Mix64(params_.seed ^ (static_cast<std::uint64_t>(id_) << 1) ^
                                (static_cast<std::uint64_t>(iter) * 0xD1B54A32D192ED03ULL));
  const double u01 = static_cast<double>(h & 0xFFFFULL) / 65535.0;

  const bool strong = (coverage >= 0.60 && confidence >= 0.50);
  const bool borderline = (coverage >= 0.45 && confidence >= 0.35 && u01 > 0.70);
  return (strong || borderline) ? DecideAction::Stop : DecideAction::Continue;
}

void Workflow::PopulatePreferenceListForNode(Node& n) {
  if (!provider_config_) return;
  n.preference_list.clear();
  for (const auto& tc : provider_config_->tiers) {
    if (n.resource_class == ResourceClass::embed && tc.provider == "embed_provider") {
      ExecutionOption opt;
      opt.provider = tc.provider;
      opt.tier_id = tc.tier_id;
      opt.price_per_call = tc.price_per_call;
      opt.timeout_ms = tc.default_timeout_ms;
      opt.max_retries = tc.default_max_retries;
      n.preference_list.push_back(opt);
    } else if (n.resource_class == ResourceClass::llm && tc.provider == "llm_provider") {
      ExecutionOption opt;
      opt.provider = tc.provider;
      opt.tier_id = tc.tier_id;
      opt.price_per_call = tc.price_per_call;
      opt.timeout_ms = tc.default_timeout_ms;
      opt.max_retries = tc.default_max_retries;
      n.preference_list.push_back(opt);
    }
  }
  std::sort(n.preference_list.begin(), n.preference_list.end(),
            [](const ExecutionOption& a, const ExecutionOption& b) {
              return a.price_per_call < b.price_per_call;
            });
}

void Workflow::OnDecideNext(NodeId decide_node) {
  const Node& dn = node(decide_node);
  const int iter = dn.iter;

  const DecideAction action = ComputeDecideAction(iter);
  if (action == DecideAction::Stop) {
    done_ = true;
    stop_iter_ = iter;
    PruneAfterStop(iter);
    return;
  }

  const int next_iter = iter + 1;
  Node plan;
  plan.id = NewNodeId();
  plan.workflow_id = id_;
  plan.type = NodeType::Plan;
  plan.resource_class = ResourceClass::llm;
  plan.idempotent = true;
  plan.iter = next_iter;
  plan.output_size_est = static_cast<std::size_t>(220 + 15 * params_.subqueries_per_iter + 4 * params_.pdfs);

  const NodeId plan_id = plan.id;
  Node& plan_ref = AddNode(std::move(plan));
  PopulatePreferenceListForNode(plan_ref);
  AddEdge(decide_node, plan_id);
  InitializeStateFromDeps(plan_id);
}

}  // namespace sim

