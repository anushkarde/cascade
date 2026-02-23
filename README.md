# agent_sched_sim

DAG-aware scheduling simulator for multi-iteration research agent workflows over mocked external providers. Implements critical-path-first prioritization, tier escalation, straggler hedging, and tail-latency mitigation.

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

## Run

```bash
./build/agent_sched_sim --workflows 100 --pdfs 10 --iters 3 --subqueries 4 --policy full --seed 1 --time_scale 50 --out_dir out
```

## Policies

- `fifo_cheapest`: FIFO per queue, always choose cheapest tier
- `dag_cheapest`: DAG-aware priority (critical-path-first), cheapest tier only
- `dag_escalation`: DAG-aware + congestion-aware tier escalation, no hedging
- `full`: DAG-aware + tier escalation + straggler hedging and cancellation

## Ablation flags

- `--disable_hedging`: Turn off hedged duplicate requests (shows tail-latency impact)
- `--disable_escalation`: Turn off tier escalation (shows congestion handling)
- `--disable_dag_priority`: Turn off DAG-aware prioritization (shows head-of-line blocking)

## Evaluation presets

### Workload scale (N workflows)

```bash
# N=50
./build/agent_sched_sim --workflows 50 --pdfs 10 --iters 3 --subqueries 4 --policy full --seed 1 --time_scale 50 --out_dir out_50

# N=100
./build/agent_sched_sim --workflows 100 --pdfs 10 --iters 3 --subqueries 4 --policy full --seed 1 --time_scale 50 --out_dir out_100

# N=500
./build/agent_sched_sim --workflows 500 --pdfs 10 --iters 3 --subqueries 4 --policy full --seed 1 --time_scale 100 --out_dir out_500
```

### Ablation sweep

```bash
# Baseline: FIFO, cheapest
./build/agent_sched_sim --workflows 100 --policy fifo_cheapest --out_dir out_fifo

# DAG-aware, cheapest
./build/agent_sched_sim --workflows 100 --policy dag_cheapest --out_dir out_dag_cheap

# DAG + escalation, no hedging
./build/agent_sched_sim --workflows 100 --policy dag_escalation --out_dir out_dag_esc

# Full system
./build/agent_sched_sim --workflows 100 --policy full --out_dir out_full

# Full with hedging disabled (tail-latency impact)
./build/agent_sched_sim --workflows 100 --policy full --disable_hedging --out_dir out_no_hedge
```

### Heavy-tail injection

```bash
# 5% of tasks get 50x latency multiplier
./build/agent_sched_sim --workflows 100 --policy full --heavy_tail_prob 0.05 --heavy_tail_mult 50 --out_dir out_heavy
```

## Outputs

- `out/workflows.csv`: Per-workflow makespan, cost, retries, cancellations, hedges, wasted_ms
- `out/tiers.csv`: Per-tier utilization, queue wait p95
- `out/summary.csv`: Aggregate mean/p50/p95/p99 makespan and cost
- `out/trace.json`: Event stream (NodeQueued, AttemptFinish, AttemptFail, HedgeLaunched, WorkflowDone)

## Options

```
--workflows N         Number of workflows (default: 100)
--pdfs N              PDFs per workflow (default: 10)
--iters N             Max iterations (default: 3)
--subqueries N        Subqueries per iteration (default: 4)
--policy NAME         fifo_cheapest | dag_cheapest | dag_escalation | full
--seed N              RNG seed (default: 1)
--time_scale N        Divide all sleeps by N for faster runs (default: 50)
--out_dir PATH        Output directory (default: out)
--heavy_tail_prob N   Fraction of tasks with heavy-tail latency (default: 0.02)
--heavy_tail_mult N   Latency multiplier for heavy-tail tasks (default: 50)
```
