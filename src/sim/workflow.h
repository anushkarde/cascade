#pragma once

#include "sim/types.h"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace sim {

struct WorkloadParams {
  int pdfs = 10;
  int subqueries_per_iter = 4;
  int max_iters = 3;
  std::uint64_t seed = 1;
};

enum class DecideAction { Stop, Continue };

class Workflow {
 public:
  Workflow(WorkflowId id, WorkloadParams params);

  WorkflowId id() const { return id_; }
  const WorkloadParams& params() const { return params_; }

  const std::unordered_map<NodeId, Node>& nodes() const { return graph_.nodes; }
  std::unordered_map<NodeId, Node>& nodes_mut() { return graph_.nodes; }

  const Node& node(NodeId nid) const;
  Node& node_mut(NodeId nid);

  bool done() const { return done_; }
  int completed_iters() const { return completed_iters_; }
  std::optional<int> stop_iter() const { return stop_iter_; }

  // Returns nodes that became runnable due to dependency satisfaction.
  std::vector<NodeId> RefreshRunnable();
  std::vector<NodeId> RunnableNodes() const;

  // State machine transitions used by a controller/scheduler.
  void MarkQueued(NodeId nid);
  void MarkRunning(NodeId nid);
  void MarkSucceeded(NodeId nid);
  void MarkFailed(NodeId nid);
  void Cancel(NodeId nid);

  // Best-effort pruning hook: cancels all non-terminal nodes in branches that are no longer needed.
  void PruneAfterStop(int stop_iter);

 private:
  NodeId NewNodeId();
  Node& AddNode(Node n);
  void AddEdge(NodeId from, NodeId to);  // from -> to

  void SetState(NodeId nid, NodeState next);
  bool DepsSatisfied(const Node& n) const;
  void InitializeStateFromDeps(NodeId nid);

  void EnsureInitialPlan();
  void ExpandIterationFromPlan(NodeId plan_node);
  void OnDecideNext(NodeId decide_node);
  DecideAction ComputeDecideAction(int iter) const;

  int IterEvidenceTotal(int iter) const;
  int IterPdfCoverageCount(int iter) const;

  WorkflowId id_ = 0;
  WorkloadParams params_;
  WorkflowGraph graph_;

  bool done_ = false;
  int completed_iters_ = 0;
  std::optional<int> stop_iter_;
};

}  // namespace sim

