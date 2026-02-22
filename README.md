# agent_sched_sim

Threaded execution/scheduling simulator for iterative retrieval-and-summarize research workflows modeled as DAGs.

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

## Run

```bash
./build/agent_sched_sim --workflows 100 --pdfs 10 --iters 3 --subqueries 4 --policy full --seed 1 --time_scale 50 --out_dir out
```

For all flags:

```bash
./build/agent_sched_sim --help
```

