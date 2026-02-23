#pragma once

#include "sim/types.h"

#include <mutex>
#include <ostream>
#include <string>

namespace sim {

enum class TraceEvent {
  NodeRunnable,
  NodeQueued,
  AttemptStart,
  AttemptFinish,
  AttemptFail,
  AttemptCancel,
  HedgeLaunched,
  WorkflowDone,
};

class TraceWriter {
 public:
  explicit TraceWriter(std::ostream& out);
  ~TraceWriter();

  void Emit(TraceEvent ev, double time_ms, WorkflowId wf_id, NodeId node_id,
            const std::string& extra = "");

 private:
  std::ostream& out_;
  std::mutex mutex_;
  bool first_ = true;
};

}  // namespace sim
