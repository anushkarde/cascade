from __future__ import annotations

import argparse
import csv
import json
import re
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence


REPO_ROOT = Path(__file__).resolve().parents[1]


def _load_json(path: Path) -> Dict[str, Any]:
  with path.open("r", encoding="utf-8") as f:
    return json.load(f)


def _read_single_row_csv(path: Path) -> Dict[str, Any]:
  with path.open("r", encoding="utf-8", newline="") as f:
    reader = csv.DictReader(f)
    rows = list(reader)
  if not rows:
    return {}
  if len(rows) > 1:
    # summary.csv is expected to have 1 row; keep the first if multiple.
    return dict(rows[0])
  return dict(rows[0])


def _read_tiers_wide(path: Path) -> Dict[str, Any]:
  out: Dict[str, Any] = {}
  with path.open("r", encoding="utf-8", newline="") as f:
    reader = csv.DictReader(f)
    for row in reader:
      provider = str(row.get("provider", "provider"))
      tier_id = str(row.get("tier_id", "x"))
      key_prefix = f"{_sanitize(provider)}_tier{tier_id}"
      for metric in ("utilization", "queue_wait_p95_ms", "in_flight_avg"):
        if metric in row and row[metric] is not None:
          out[f"{key_prefix}_{metric}"] = row[metric]
  return out


def _parse_ablation_flags(flags: Optional[List[str]]) -> Dict[str, bool]:
  """Extract disable_* booleans from ablation flags for easier filtering."""
  out: Dict[str, bool] = {
    "disable_hedging": False,
    "disable_escalation": False,
    "disable_dag_priority": False,
  }
  if not flags:
    return out
  for f in flags:
    s = str(f).strip()
    if s == "--disable_hedging":
      out["disable_hedging"] = True
    elif s == "--disable_escalation":
      out["disable_escalation"] = True
    elif s == "--disable_dag_priority":
      out["disable_dag_priority"] = True
  return out


_SAN_RE = re.compile(r"[^a-zA-Z0-9_]+")


def _sanitize(s: str) -> str:
  s2 = s.replace("-", "_").replace(".", "_")
  s2 = _SAN_RE.sub("_", s2)
  return s2.strip("_") or "x"


def _flatten(prefix: str, d: Dict[str, Any], out: Dict[str, Any]) -> None:
  for k, v in d.items():
    key = f"{prefix}{k}" if prefix else str(k)
    if isinstance(v, dict):
      _flatten(key + ".", v, out)
    else:
      out[key] = v


def _write_rows_csv(path: Path, rows: Sequence[Dict[str, Any]]) -> None:
  cols: set[str] = set()
  for r in rows:
    cols.update(r.keys())

  preferred = [
    "run_id",
    "policy",
    "workflows",
    "pdfs",
    "iters",
    "subqueries",
    "seed",
    "time_scale",
    "enable_model_routing",
    "heavy_tail_prob",
    "heavy_tail_mult",
    "ablation_name",
    "ablation_flags",
    "disable_hedging",
    "disable_escalation",
    "disable_dag_priority",
    "exit_code",
    "wall_time_s",
    "sim_binary_version",
    "makespan_mean_ms",
    "makespan_p50_ms",
    "makespan_p95_ms",
    "makespan_p99_ms",
    "cost_mean",
    "cost_p50",
    "run_dir",
    "start_time_unix_s",
    "end_time_unix_s",
  ]
  ordered = [c for c in preferred if c in cols] + sorted([c for c in cols if c not in preferred])

  path.parent.mkdir(parents=True, exist_ok=True)
  with path.open("w", encoding="utf-8", newline="") as f:
    w = csv.DictWriter(f, fieldnames=ordered)
    w.writeheader()
    for r in rows:
      w.writerow({k: ("" if r.get(k) is None else r.get(k)) for k in ordered})


def main(argv: Optional[List[str]] = None) -> int:
  p = argparse.ArgumentParser(description="Collect bench sweep outputs into a tidy dataset.")
  p.add_argument(
    "--exp_dir",
    required=True,
    help="Experiment directory, e.g. bench_runs/my_exp",
  )
  p.add_argument(
    "--out_csv",
    default=None,
    help="Output CSV path (defaults to <exp_dir>/aggregate/runs.csv).",
  )
  args = p.parse_args(argv)

  exp_dir = Path(args.exp_dir).expanduser()
  if not exp_dir.is_absolute():
    exp_dir = (REPO_ROOT / exp_dir).resolve()

  runs_dir = exp_dir / "runs"
  if not runs_dir.exists():
    raise SystemExit(f"runs directory not found: {runs_dir}")

  rows: List[Dict[str, Any]] = []
  for run_dir in sorted([p for p in runs_dir.iterdir() if p.is_dir()]):
    meta_path = run_dir / "meta.json"
    out_dir = run_dir / "out"
    summary_path = out_dir / "summary.csv"
    tiers_path = out_dir / "tiers.csv"

    if not meta_path.exists():
      continue
    meta = _load_json(meta_path)

    row: Dict[str, Any] = {}
    row["run_id"] = meta.get("run_id", run_dir.name)
    row["run_dir"] = str(run_dir)
    row["exit_code"] = meta.get("exit_code")
    row["wall_time_s"] = meta.get("wall_time_s")
    row["start_time_unix_s"] = meta.get("start_time_unix_s")
    row["end_time_unix_s"] = meta.get("end_time_unix_s")

    row["ablation_name"] = meta.get("ablation_name")
    ablation_flags = meta.get("ablation_flags") or []
    if ablation_flags:
      row["ablation_flags"] = " ".join(str(x) for x in ablation_flags)
    for k, v in _parse_ablation_flags(ablation_flags).items():
      row[k] = v

    if meta.get("sim_binary_version") is not None:
      row["sim_binary_version"] = meta["sim_binary_version"]

    params = meta.get("params") or {}
    if isinstance(params, dict):
      _flatten("", params, row)

    if summary_path.exists():
      summary = _read_single_row_csv(summary_path)
      for k, v in summary.items():
        row[k] = v
    if tiers_path.exists():
      row.update(_read_tiers_wide(tiers_path))

    rows.append(row)

  if not rows:
    raise SystemExit(f"no runs found under: {runs_dir}")

  agg_dir = exp_dir / "aggregate"
  agg_dir.mkdir(parents=True, exist_ok=True)
  out_csv = Path(args.out_csv).expanduser() if args.out_csv else (agg_dir / "runs.csv")
  if not out_csv.is_absolute():
    out_csv = (REPO_ROOT / out_csv).resolve()

  _write_rows_csv(out_csv, rows)
  print(f"wrote: {out_csv} rows={len(rows)}")
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
