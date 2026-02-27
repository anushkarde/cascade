# Benchmark Sweep Runner

Runs the agent_sched_sim across an experiment matrix (policies, ablations, seeds, workload sizes) with bounded concurrency and structured outputs.

## Prerequisites

1. Build the simulator: `./build/agent_sched_sim` must exist.
2. Python dependencies (for aggregation and report generation):
   ```bash
   pip install -r bench/requirements.txt
   ```
   (`pandas`, `matplotlib`)

## Quick start

```bash
# Dry run: print planned runs without executing
python bench/sweep.py --dry_run

# Run with 2 concurrent jobs (default from config)
python bench/sweep.py --jobs 2

# Resume: skip runs that already have out/summary.csv and matching meta.json
python bench/sweep.py --jobs 2 --resume

# Fail fast: stop on first failing run (default is keep going)
python bench/sweep.py --jobs 2 --fail_fast
```

## Full workflow

```bash
# 1. Run sweep (uses bench/experiments/default.json by default)
python bench/sweep.py --config bench/experiments/smoke.json --jobs 2

# 2. Aggregate all runs into runs.csv
python bench/collect.py --exp_dir bench_runs/smoke

# 3. Generate static HTML report + plots
python bench/report.py --exp_dir bench_runs/smoke
```

Report is written to `bench_runs/<exp_name>/report/index.html`. Open in a browser to view.

## Output layout

```
bench_runs/<exp_name>/
  runs/
    <run_id>/
      meta.json       # params, ablation, cmd, start/end, wall_time_s, exit_code
      stdout.txt
      stderr.txt
      out/
        summary.csv   # per-run metrics (makespan, cost, etc.)
        workflows.csv
        tiers.csv     # per-tier queue wait, utilization
        trace.json
  aggregate/
    runs.csv          # merged meta + summary + tiers (1 row per run)
  report/
    index.html        # static report with plots and sortable table
    assets/
      makespan_by_policy.png
      cost_by_policy.png
      ablation_deltas_full_p95_ratio.png
```

### Per-run files

| File | Description |
|------|-------------|
| `meta.json` | Full parameterization, ablation name/flags, command, start/end timestamps, wall time, exit code |
| `stdout.txt` | Simulator stdout |
| `stderr.txt` | Simulator stderr |
| `out/summary.csv` | Single-row metrics: makespan_mean_ms, makespan_p50/p95/p99_ms, cost_mean, cost_p50 |
| `out/workflows.csv` | Per-workflow metrics |
| `out/tiers.csv` | Per-tier stats: provider, tier_id, queue_wait_p95_ms, utilization, in_flight_avg |
| `out/trace.json` | Event trace (can be large) |

### Aggregate runs.csv

One row per run. Columns include:

- **Config**: `run_id`, `policy`, `workflows`, `pdfs`, `iters`, `subqueries`, `seed`, `time_scale`, `enable_model_routing`, `heavy_tail_prob`, `heavy_tail_mult`, `ablation_name`, `ablation_flags`, `disable_hedging`, `disable_escalation`, `disable_dag_priority`
- **Metrics**: `makespan_mean_ms`, `makespan_p50_ms`, `makespan_p95_ms`, `makespan_p99_ms`, `cost_mean`, `cost_p50`
- **Tier metrics**: `llm_provider_tier0_queue_wait_p95_ms`, `embed_provider_tier0_queue_wait_p95_ms`, etc.
- **Runtime**: `exit_code`, `wall_time_s`, `run_dir`, `start_time_unix_s`, `end_time_unix_s`

## Experiment config

Config path defaults to `bench/experiments/default.json`. Override with `--config`.

Example structure:

```json
{
  "name": "default",
  "sim_binary": "./build/agent_sched_sim",
  "out_root": "bench_runs",
  "common_args": {
    "pdfs": 10,
    "iters": 3,
    "subqueries": 4,
    "time_scale": 50,
    "enable_model_routing": true,
    "heavy_tail_prob": 0.02,
    "heavy_tail_mult": 50
  },
  "matrix": {
    "workflows": [50, 100],
    "policy": ["fifo_cheapest", "dag_cheapest", "dag_escalation", "full"],
    "seeds": [1, 2, 3],
    "ablations": [
      {"name": "baseline", "flags": []},
      {"name": "no_hedge", "flags": ["--disable_hedging"]},
      {"name": "no_escalation", "flags": ["--disable_escalation"]},
      {"name": "no_dag_priority", "flags": ["--disable_dag_priority"]}
    ]
  },
  "jobs": 2
}
```

- **matrix**: Cartesian product over all keys except `seeds` and `ablations`, which are expanded separately. Each run = one combo x one seed x one ablation.
- **ablations**: Each entry has `name` and `flags` (CLI flags passed to the sim).

Use `bench/experiments/smoke.json` for a quick validation sweep (2 policies x 2 ablations x 2 seeds, workflows=5).

## CLI reference

### sweep.py

| Option | Description |
|--------|-------------|
| `--config PATH` | Experiment JSON (default: bench/experiments/default.json) |
| `--exp_name NAME` | Override experiment name |
| `--out_root PATH` | Override output root (default: bench_runs) |
| `--jobs N` | Max concurrent runs |
| `--dry_run` | Print planned runs only |
| `--resume` | Skip runs with existing out/summary.csv and matching meta.json |
| `--fail_fast` | Stop on first failing run |

### collect.py

| Option | Description |
|--------|-------------|
| `--exp_dir PATH` | Experiment directory (required), e.g. bench_runs/smoke |
| `--out_csv PATH` | Output CSV path (default: <exp_dir>/aggregate/runs.csv) |

### report.py

| Option | Description |
|--------|-------------|
| `--exp_dir PATH` | Experiment directory (required), e.g. bench_runs/smoke |
| `--runs_csv PATH` | Aggregate CSV path (default: <exp_dir>/aggregate/runs.csv) |
| `--out_dir PATH` | Report output dir (default: <exp_dir>/report) |

## Report contents

The generated HTML report includes:

- Summary: run count, policies, workflow scales, ablations
- Plots (PNG): makespan p50/p95/p99 by policy (faceted by workload), cost mean by policy, ablation deltas vs full baseline (p95 makespan ratio)
- Sortable table of run-level results (click headers to sort)
