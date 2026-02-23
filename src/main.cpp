#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "sim/provider.h"
#include "sim/workflow.h"

namespace fs = std::filesystem;

enum class Policy {
  fifo_cheapest,
  dag_cheapest,
  dag_escalation,
  full,
};

struct CliOptions {
  int workflows = 100;
  int pdfs = 10;
  int iters = 3;
  int subqueries = 4;
  Policy policy = Policy::full;
  std::uint64_t seed = 1;
  int time_scale = 50;
  std::string out_dir = "out";

  bool disable_hedging = false;
  bool disable_escalation = false;
  bool disable_dag_priority = false;
};

static std::string ToString(Policy p) {
  switch (p) {
    case Policy::fifo_cheapest: return "fifo_cheapest";
    case Policy::dag_cheapest: return "dag_cheapest";
    case Policy::dag_escalation: return "dag_escalation";
    case Policy::full: return "full";
  }
  return "unknown";
}

static std::optional<Policy> ParsePolicy(std::string_view s) {
  if (s == "fifo_cheapest") return Policy::fifo_cheapest;
  if (s == "dag_cheapest") return Policy::dag_cheapest;
  if (s == "dag_escalation") return Policy::dag_escalation;
  if (s == "full") return Policy::full;
  return std::nullopt;
}

static void PrintUsage(std::ostream& os, const char* argv0) {
  os << "Usage:\n"
     << "  " << argv0
     << " --workflows N --pdfs N --iters N --subqueries N --policy <name> --seed N --time_scale N --out_dir PATH [flags]\n"
     << "\n"
     << "Options:\n"
     << "  --workflows N         Number of workflows (default: 100)\n"
     << "  --pdfs N              PDFs per workflow (default: 10)\n"
     << "  --iters N             Max iterations (default: 3)\n"
     << "  --subqueries N        Subqueries per iteration (default: 4)\n"
     << "  --policy NAME         One of: fifo_cheapest, dag_cheapest, dag_escalation, full (default: full)\n"
     << "  --seed N              RNG seed (default: 1)\n"
     << "  --time_scale N        Divide all sleeps by N (>=1) (default: 50)\n"
     << "  --out_dir PATH        Output directory (default: out)\n"
     << "\n"
     << "Flags:\n"
     << "  --disable_hedging\n"
     << "  --disable_escalation\n"
     << "  --disable_dag_priority\n"
     << "  -h, --help            Show this help\n";
}

static std::string RequireValue(const std::vector<std::string>& args, std::size_t i) {
  if (i + 1 >= args.size()) {
    throw std::runtime_error("Missing value for argument: " + args[i]);
  }
  return args[i + 1];
}

static int ParseInt(const std::string& s, const std::string& flag_name) {
  std::size_t idx = 0;
  int v = 0;
  try {
    v = std::stoi(s, &idx, 10);
  } catch (const std::exception&) {
    throw std::runtime_error("Invalid integer for " + flag_name + ": " + s);
  }
  if (idx != s.size()) {
    throw std::runtime_error("Invalid integer for " + flag_name + ": " + s);
  }
  return v;
}

static std::uint64_t ParseU64(const std::string& s, const std::string& flag_name) {
  std::size_t idx = 0;
  unsigned long long v = 0;
  try {
    v = std::stoull(s, &idx, 10);
  } catch (const std::exception&) {
    throw std::runtime_error("Invalid integer for " + flag_name + ": " + s);
  }
  if (idx != s.size()) {
    throw std::runtime_error("Invalid integer for " + flag_name + ": " + s);
  }
  return static_cast<std::uint64_t>(v);
}

static void Validate(const CliOptions& o) {
  auto require_pos = [](int v, const char* name) {
    if (v <= 0) throw std::runtime_error(std::string(name) + " must be > 0");
  };
  require_pos(o.workflows, "workflows");
  require_pos(o.pdfs, "pdfs");
  require_pos(o.iters, "iters");
  if (o.subqueries < 0) throw std::runtime_error("subqueries must be >= 0");
  require_pos(o.time_scale, "time_scale");
  if (o.out_dir.empty()) throw std::runtime_error("out_dir must be non-empty");
}

static CliOptions ParseArgs(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);

  CliOptions o;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "-h" || a == "--help") {
      PrintUsage(std::cout, argv[0]);
      std::exit(0);
    }

    if (a == "--disable_hedging") {
      o.disable_hedging = true;
      continue;
    }
    if (a == "--disable_escalation") {
      o.disable_escalation = true;
      continue;
    }
    if (a == "--disable_dag_priority") {
      o.disable_dag_priority = true;
      continue;
    }

    if (a == "--workflows") {
      o.workflows = ParseInt(RequireValue(args, i), a);
      ++i;
      continue;
    }
    if (a == "--pdfs") {
      o.pdfs = ParseInt(RequireValue(args, i), a);
      ++i;
      continue;
    }
    if (a == "--iters") {
      o.iters = ParseInt(RequireValue(args, i), a);
      ++i;
      continue;
    }
    if (a == "--subqueries") {
      o.subqueries = ParseInt(RequireValue(args, i), a);
      ++i;
      continue;
    }
    if (a == "--seed") {
      o.seed = ParseU64(RequireValue(args, i), a);
      ++i;
      continue;
    }
    if (a == "--time_scale") {
      o.time_scale = ParseInt(RequireValue(args, i), a);
      ++i;
      continue;
    }
    if (a == "--out_dir") {
      o.out_dir = RequireValue(args, i);
      ++i;
      continue;
    }
    if (a == "--policy") {
      auto p = ParsePolicy(RequireValue(args, i));
      if (!p) throw std::runtime_error("Unknown policy: " + RequireValue(args, i));
      o.policy = *p;
      ++i;
      continue;
    }

    throw std::runtime_error("Unknown argument: " + a);
  }

  Validate(o);
  return o;
}

static int RunSimulation(const CliOptions& o) {
  std::error_code ec;
  fs::create_directories(o.out_dir, ec);
  if (ec) {
    throw std::runtime_error("Failed to create out_dir '" + o.out_dir + "': " + ec.message());
  }

  std::cout << "agent_sched_sim config:\n"
            << "  workflows=" << o.workflows << "\n"
            << "  pdfs=" << o.pdfs << "\n"
            << "  iters=" << o.iters << "\n"
            << "  subqueries=" << o.subqueries << "\n"
            << "  policy=" << ToString(o.policy) << "\n"
            << "  seed=" << o.seed << "\n"
            << "  time_scale=" << o.time_scale << "\n"
            << "  out_dir=" << o.out_dir << "\n"
            << "  disable_hedging=" << (o.disable_hedging ? "true" : "false") << "\n"
            << "  disable_escalation=" << (o.disable_escalation ? "true" : "false") << "\n"
            << "  disable_dag_priority=" << (o.disable_dag_priority ? "true" : "false") << "\n";

  // Bring-up sanity check: build multi-iteration workflow DAGs, execute them with a trivial
  // "instant success" controller, and verify the state machine can reach termination.
  sim::WorkloadParams wp;
  wp.pdfs = o.pdfs;
  wp.subqueries_per_iter = o.subqueries;
  wp.max_iters = o.iters;
  wp.seed = o.seed;

  std::uint64_t total_nodes = 0;
  std::uint64_t total_cancelled = 0;
  int max_completed_iters = 0;

  for (int i = 0; i < o.workflows; ++i) {
    sim::Workflow wf(static_cast<sim::WorkflowId>(i + 1), wp);

    std::uint64_t safety = 0;
    while (!wf.done()) {
      wf.RefreshRunnable();
      const auto runnable = wf.RunnableNodes();
      if (runnable.empty()) {
        throw std::runtime_error("Workflow deadlocked (no runnable nodes, not done)");
      }
      const sim::NodeId nid = runnable.front();
      wf.MarkRunning(nid);
      wf.MarkSucceeded(nid);

      if (++safety > 5'000'000ULL) {
        throw std::runtime_error("Workflow did not converge (safety cap exceeded)");
      }
    }

    total_nodes += wf.nodes().size();
    for (const auto& [nid, n] : wf.nodes()) {
      if (n.state == sim::NodeState::Cancelled) ++total_cancelled;
    }
    max_completed_iters = std::max(max_completed_iters, wf.completed_iters());
  }

  std::cout << "workflow generator sanity:\n"
            << "  total_workflows=" << o.workflows << "\n"
            << "  total_nodes=" << total_nodes << "\n"
            << "  total_cancelled=" << total_cancelled << "\n"
            << "  max_completed_iters=" << max_completed_iters << "\n";

  // Provider tier sanity: token bucket, queues, latency sampling
  sim::ProviderConfig prov_config;
  sim::ProviderManager prov_mgr(prov_config);
  sim::SeededRng rng(o.seed);
  sim::LatencySampler sampler(prov_config.latency, &rng);

  int samples_ok = 0;
  int samples_fail = 0;
  int samples_timeout = 0;
  for (int i = 0; i < 50; ++i) {
    sim::LatencyContext ctx;
    ctx.node_type = (i % 2 == 0) ? sim::NodeType::Embed : sim::NodeType::Plan;
    ctx.token_length_est = 100 + static_cast<std::size_t>(i) * 10;
    auto s = sampler.Sample(ctx, 30'000, 0.02);
    if (s.failed) ++samples_fail;
    else if (s.timeout) ++samples_timeout;
    else ++samples_ok;
  }

  sim::Tier* tier = prov_mgr.GetTier("embed_provider", 0);
  if (!tier) throw std::runtime_error("embed_provider tier 0 not found");
  if (tier->concurrency_cap() != 4) throw std::runtime_error("concurrency_cap mismatch");

  std::cout << "provider sanity:\n"
            << "  latency_samples_ok=" << samples_ok << "\n"
            << "  latency_samples_fail=" << samples_fail << "\n"
            << "  latency_samples_timeout=" << samples_timeout << "\n"
            << "  tier_embed_0_concurrency=" << tier->concurrency_cap() << "\n";

  return 0;
}

int main(int argc, char** argv) {
  try {
    CliOptions o = ParseArgs(argc, argv);
    return RunSimulation(o);
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n\n";
    PrintUsage(std::cerr, argv[0]);
    return 2;
  }
}

