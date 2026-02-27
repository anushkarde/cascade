from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass
from itertools import product
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


REPO_ROOT = Path(__file__).resolve().parents[1]


def _load_json(path: Path) -> Dict[str, Any]:
  with path.open("r", encoding="utf-8") as f:
    return json.load(f)


def _json_dumps_stable(obj: Any) -> str:
  return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def _sanitize_token(s: str) -> str:
  out = []
  for ch in s:
    if ch.isalnum() or ch in ("-", "_", "."):
      out.append(ch)
    else:
      out.append("_")
  return "".join(out).strip("_")


def _stable_run_id(params: Dict[str, Any], ablation_name: str, ablation_flags: List[str]) -> str:
  payload = {
    "params": params,
    "ablation_name": ablation_name,
    "ablation_flags": ablation_flags,
  }
  h = hashlib.sha1(_json_dumps_stable(payload).encode("utf-8")).hexdigest()[:12]
  policy = _sanitize_token(str(params.get("policy", "unknown")))
  workflows = params.get("workflows", "x")
  seed = params.get("seed", "x")
  ab = _sanitize_token(ablation_name)
  return f"{policy}__wf{workflows}__seed{seed}__{ab}__{h}"


def _bool_flag(enabled: bool, flag: str) -> List[str]:
  return [flag] if enabled else []


def _build_cmd(sim_binary: str, params: Dict[str, Any], out_dir: Path, ablation_flags: List[str]) -> List[str]:
  cmd: List[str] = [sim_binary]
  cmd += ["--workflows", str(params["workflows"])]
  cmd += ["--pdfs", str(params["pdfs"])]
  cmd += ["--iters", str(params["iters"])]
  cmd += ["--subqueries", str(params["subqueries"])]
  cmd += ["--policy", str(params["policy"])]
  cmd += ["--seed", str(params["seed"])]
  cmd += ["--time_scale", str(params["time_scale"])]
  cmd += ["--heavy_tail_prob", str(params["heavy_tail_prob"])]
  cmd += ["--heavy_tail_mult", str(params["heavy_tail_mult"])]
  cmd += _bool_flag(bool(params.get("enable_model_routing", False)), "--enable_model_routing")
  cmd += list(ablation_flags)
  cmd += ["--out_dir", str(out_dir)]
  return cmd


def _expand_runs(cfg: Dict[str, Any]) -> List[Tuple[Dict[str, Any], str, List[str]]]:
  common: Dict[str, Any] = dict(cfg.get("common_args", {}))
  matrix: Dict[str, Any] = dict(cfg.get("matrix", {}))

  ablations = matrix.pop("ablations", [{"name": "baseline", "flags": []}])
  if not isinstance(ablations, list) or not ablations:
    raise ValueError("matrix.ablations must be a non-empty list")

  seeds = matrix.pop("seeds", [common.get("seed", 1)])
  if not isinstance(seeds, list) or not seeds:
    raise ValueError("matrix.seeds must be a non-empty list")

  keys = sorted(matrix.keys())
  value_lists: List[List[Any]] = []
  for k in keys:
    v = matrix[k]
    if not isinstance(v, list) or not v:
      raise ValueError(f"matrix.{k} must be a non-empty list")
    value_lists.append(v)

  runs: List[Tuple[Dict[str, Any], str, List[str]]] = []
  for combo in product(*value_lists) if value_lists else [()]:
    base_params = dict(common)
    for k, v in zip(keys, combo):
      base_params[k] = v
    for seed in seeds:
      params = dict(base_params)
      params["seed"] = seed
      for ab in ablations:
        if not isinstance(ab, dict) or "name" not in ab:
          raise ValueError("each ablation must be an object with at least a 'name'")
        name = str(ab["name"])
        flags = ab.get("flags", [])
        if flags is None:
          flags = []
        if not isinstance(flags, list):
          raise ValueError(f"ablation.flags must be a list for {name}")
        runs.append((params, name, [str(x) for x in flags]))

  required = ["workflows", "pdfs", "iters", "subqueries", "policy", "seed", "time_scale",
              "heavy_tail_prob", "heavy_tail_mult"]
  for (p, _, _) in runs:
    for k in required:
      if k not in p:
        raise ValueError(f"missing required param '{k}' (set in common_args or matrix)")
  return runs


@dataclass(frozen=True)
class RunSpec:
  params: Dict[str, Any]
  ablation_name: str
  ablation_flags: List[str]
  run_id: str
  run_dir: Path
  out_dir: Path
  cmd: List[str]


def _make_run_specs(cfg: Dict[str, Any], exp_dir: Path) -> List[RunSpec]:
  sim_binary = str(cfg.get("sim_binary", "./build/agent_sched_sim"))
  runs = _expand_runs(cfg)
  specs: List[RunSpec] = []
  for params, ablation_name, ablation_flags in runs:
    params2 = dict(params)
    params2["policy"] = str(params2["policy"])
    run_id = _stable_run_id(params2, ablation_name, ablation_flags)
    run_dir = exp_dir / "runs" / run_id
    out_dir = run_dir / "out"
    cmd = _build_cmd(sim_binary, params2, out_dir, ablation_flags)
    specs.append(RunSpec(params2, ablation_name, ablation_flags, run_id, run_dir, out_dir, cmd))
  return specs


def _meta_matches(meta: Dict[str, Any], spec: RunSpec) -> bool:
  want = {
    "params": spec.params,
    "ablation_name": spec.ablation_name,
    "ablation_flags": spec.ablation_flags,
    "cmd": spec.cmd,
  }
  have = {k: meta.get(k) for k in want.keys()}
  return _json_dumps_stable(have) == _json_dumps_stable(want)


def _should_skip(spec: RunSpec, resume: bool) -> bool:
  if not resume:
    return False
  summary = spec.out_dir / "summary.csv"
  meta_path = spec.run_dir / "meta.json"
  if not summary.exists() or not meta_path.exists():
    return False
  try:
    meta = _load_json(meta_path)
  except Exception:
    return False
  return _meta_matches(meta, spec)


def _write_json(path: Path, obj: Any) -> None:
  path.parent.mkdir(parents=True, exist_ok=True)
  tmp = path.with_suffix(path.suffix + ".tmp")
  with tmp.open("w", encoding="utf-8") as f:
    json.dump(obj, f, indent=2, sort_keys=True)
    f.write("\n")
  tmp.replace(path)


def _run_one(spec: RunSpec) -> Tuple[str, int]:
  spec.run_dir.mkdir(parents=True, exist_ok=True)
  spec.out_dir.mkdir(parents=True, exist_ok=True)

  meta_path = spec.run_dir / "meta.json"
  stdout_path = spec.run_dir / "stdout.txt"
  stderr_path = spec.run_dir / "stderr.txt"

  meta: Dict[str, Any] = {
    "run_id": spec.run_id,
    "params": spec.params,
    "ablation_name": spec.ablation_name,
    "ablation_flags": spec.ablation_flags,
    "cmd": spec.cmd,
    "cwd": str(REPO_ROOT),
    "start_time_unix_s": time.time(),
  }
  _write_json(meta_path, meta)

  t0 = time.time()
  with stdout_path.open("wb") as out_f, stderr_path.open("wb") as err_f:
    try:
      proc = subprocess.run(
        spec.cmd,
        cwd=str(REPO_ROOT),
        stdout=out_f,
        stderr=err_f,
        check=False,
      )
      code = int(proc.returncode)
    except Exception:
      code = 127
      raise
    finally:
      t1 = time.time()
      meta2 = dict(meta)
      meta2["end_time_unix_s"] = time.time()
      meta2["wall_time_s"] = round(t1 - t0, 6)
      meta2["exit_code"] = code
      _write_json(meta_path, meta2)
  return (spec.run_id, code)


def _format_cmd(cmd: List[str]) -> str:
  return " ".join(shlex.quote(x) for x in cmd)


def main(argv: Optional[List[str]] = None) -> int:
  p = argparse.ArgumentParser(description="Run benchmark sweeps for agent_sched_sim.")
  p.add_argument(
    "--config",
    default=str(REPO_ROOT / "bench" / "experiments" / "default.json"),
    help="Path to experiment JSON config.",
  )
  p.add_argument("--exp_name", default=None, help="Experiment name override (defaults to config.name).")
  p.add_argument("--out_root", default=None, help="Output root override (defaults to config.out_root).")
  p.add_argument("--jobs", type=int, default=None, help="Max concurrent runs (defaults to config.jobs or 1).")
  p.add_argument("--dry_run", action="store_true", help="Print planned runs only.")
  p.add_argument("--resume", action="store_true", help="Skip runs already completed with matching meta.json.")
  p.add_argument("--fail_fast", action="store_true", help="Stop on first failing run.")
  args = p.parse_args(argv)

  cfg_path = Path(args.config).expanduser()
  if not cfg_path.is_absolute():
    cfg_path = (REPO_ROOT / cfg_path).resolve()
  cfg = _load_json(cfg_path)

  exp_name = str(args.exp_name or cfg.get("name") or "exp")
  out_root = Path(args.out_root or cfg.get("out_root") or "bench_runs")
  if not out_root.is_absolute():
    out_root = (REPO_ROOT / out_root).resolve()

  exp_dir = out_root / _sanitize_token(exp_name)
  jobs = int(args.jobs or cfg.get("jobs") or 1)
  if jobs <= 0:
    raise ValueError("--jobs must be > 0")

  specs = _make_run_specs(cfg, exp_dir)

  to_run: List[RunSpec] = []
  skipped: List[RunSpec] = []
  for s in specs:
    if _should_skip(s, resume=bool(args.resume)):
      skipped.append(s)
    else:
      to_run.append(s)

  print(f"experiment: {exp_dir}")
  print(f"planned_runs={len(specs)} to_run={len(to_run)} skipped={len(skipped)} jobs={jobs}")
  if args.dry_run:
    try:
      for s in to_run:
        print(f"- {s.run_id}")
        print(f"  run_dir={s.run_dir}")
        print(f"  cmd={_format_cmd(s.cmd)}")
      return 0
    except BrokenPipeError:
      try:
        sys.stdout = open(os.devnull, "w")
      except Exception:
        pass
      return 0

  exp_dir.mkdir(parents=True, exist_ok=True)
  (exp_dir / "runs").mkdir(parents=True, exist_ok=True)

  failures: List[Tuple[str, int]] = []
  completed = 0

  with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
    fut_to_spec = {ex.submit(_run_one, s): s for s in to_run}
    for fut in concurrent.futures.as_completed(fut_to_spec):
      spec = fut_to_spec[fut]
      try:
        run_id, code = fut.result()
      except Exception as e:
        failures.append((spec.run_id, 127))
        print(f"[FAIL] {spec.run_id}: exception: {e}", file=sys.stderr)
        if args.fail_fast:
          for f2 in fut_to_spec:
            f2.cancel()
          break
        continue

      completed += 1
      if code == 0:
        print(f"[OK]   {run_id} ({completed}/{len(to_run)})")
      else:
        failures.append((run_id, code))
        print(f"[FAIL] {run_id} exit_code={code} ({completed}/{len(to_run)})", file=sys.stderr)
        if args.fail_fast:
          for f2 in fut_to_spec:
            f2.cancel()
          break

  if failures:
    print(f"failures={len(failures)}", file=sys.stderr)
    for rid, code in failures[:50]:
      print(f"- {rid}: exit_code={code}", file=sys.stderr)
    return 1

  return 0


if __name__ == "__main__":
  raise SystemExit(main())
