#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sim {

using WorkflowId = std::uint32_t;
using NodeId = std::uint64_t;

enum class NodeType {
  Plan,
  LoadPDF,
  Chunk,
  Embed,
  SimilaritySearch,
  ExtractEvidence,
  Aggregate,
  DecideNext,
};

enum class ResourceClass {
  cpu,
  io,
  embed,
  llm,
};

enum class NodeState {
  WaitingDeps,
  Runnable,
  Queued,
  Running,
  Succeeded,
  Failed,
  Cancelled,
};

inline bool IsTerminal(NodeState s) {
  return s == NodeState::Succeeded || s == NodeState::Failed || s == NodeState::Cancelled;
}

inline bool IsActive(NodeState s) {
  return s == NodeState::Runnable || s == NodeState::Queued || s == NodeState::Running;
}

struct ExecutionOption {
  std::string provider;
  int tier_id = 0;
  double price_per_call = 0.0;
  int timeout_ms = 0;
  int max_retries = 0;
};

struct Node {
  NodeId id = 0;
  WorkflowId workflow_id = 0;

  NodeType type = NodeType::Plan;
  ResourceClass resource_class = ResourceClass::cpu;
  bool idempotent = true;

  NodeState state = NodeState::WaitingDeps;

  // Iteration index for multi-iteration workflows.
  int iter = 0;

  // Optional metadata for the mocked retrieval graph.
  int pdf_idx = -1;
  int subquery_idx = -1;

  // Dependency edges in the DAG (node IDs within the same workflow).
  std::vector<NodeId> deps;
  std::vector<NodeId> children;

  // Used by later scheduling policies; populated for provider-backed node types.
  std::vector<ExecutionOption> preference_list;

  // Lightweight estimates/outputs to drive DecideNext deterministically in the generator.
  std::size_t output_size_est = 0;
  int evidence_count_est = 0;
};

struct WorkflowGraph {
  std::unordered_map<NodeId, Node> nodes;
  NodeId next_node_id = 1;
};

using AttemptId = std::uint64_t;

}  // namespace sim

