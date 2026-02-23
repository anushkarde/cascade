#include "sim/trace.h"

#include <sstream>

namespace sim {

static const char* EventName(TraceEvent ev) {
  switch (ev) {
    case TraceEvent::NodeRunnable: return "NodeRunnable";
    case TraceEvent::NodeQueued: return "NodeQueued";
    case TraceEvent::AttemptStart: return "AttemptStart";
    case TraceEvent::AttemptFinish: return "AttemptFinish";
    case TraceEvent::AttemptFail: return "AttemptFail";
    case TraceEvent::AttemptCancel: return "AttemptCancel";
    case TraceEvent::HedgeLaunched: return "HedgeLaunched";
    case TraceEvent::WorkflowDone: return "WorkflowDone";
  }
  return "Unknown";
}

TraceWriter::TraceWriter(std::ostream& out) : out_(out) {
  std::lock_guard lock(mutex_);
  out_ << "[\n";
}

TraceWriter::~TraceWriter() {
  std::lock_guard lock(mutex_);
  out_ << "\n]\n";
}

void TraceWriter::Emit(TraceEvent ev, double time_ms, WorkflowId wf_id, NodeId node_id,
                      const std::string& extra) {
  std::lock_guard lock(mutex_);
  if (!first_) out_ << ",\n";
  first_ = false;
  out_ << "  {\"ev\":\"" << EventName(ev) << "\",\"t_ms\":" << time_ms << ",\"wf\":" << wf_id
       << ",\"node\":" << node_id;
  if (!extra.empty()) out_ << ",\"extra\":\"" << extra << "\"";
  out_ << "}";
}

}  // namespace sim
