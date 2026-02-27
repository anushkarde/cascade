# Benchmark Sweep Runner

Runs the agent_sched_sim across an experiment matrix (policies, ablations, seeds, workload sizes) with bounded concurrency and structured outputs.

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

## Output layout

```
bench_runs/<exp_name>/
  runs/
    <run_id>/
      meta.json       # params, ablation, cmd, start/end, wall_time_s, exit_code
      stdout.txt
      stderr.txt
      out/
        summary.csv
        workflows.csv
        tiers.csv
        trace.json
```

## CLI options

| Option | Description |
|--------|-------------|
| `--config PATH` | Experiment JSON (default: bench/experiments/default.json) |
| `--exp_name NAME` | Override experiment name |
| `--out_root PATH` | Override output root (default: bench_runs) |
| `--jobs N` | Max concurrent runs |
| `--dry_run` | Print planned runs only |
| `--resume` | Skip runs with existing out/summary.csv and matching meta.json |
| `--fail_fast` | Stop on first failing run |

## Aggregation and report

After running a sweep:

```bash
# Aggregate all runs into runs.csv
python bench/collect.py --exp_dir bench_runs/<exp_name>

# Generate static HTML report + plots
python bench/report.py --exp_dir bench_runs/<exp_name>
```

Report is written to `bench_runs/<exp_name>/report/index.html`.
