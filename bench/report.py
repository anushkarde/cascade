from __future__ import annotations

import argparse
import csv
import html
from pathlib import Path
from typing import List, Optional


REPO_ROOT = Path(__file__).resolve().parents[1]


def _read_csv_rows(path: Path) -> tuple[list[str], list[dict[str, str]]]:
  with path.open("r", encoding="utf-8", newline="") as f:
    r = csv.DictReader(f)
    headers = [h for h in (r.fieldnames or [])]
    rows: list[dict[str, str]] = []
    for row in r:
      rows.append({k: ("" if row.get(k) is None else str(row.get(k))) for k in headers})
  return headers, rows


def _try_load_pandas_df(path: Path):
  try:
    import pandas as pd  # type: ignore
  except Exception:
    return None
  return pd.read_csv(path)


def _try_enable_matplotlib() -> bool:
  try:
    import matplotlib
    matplotlib.use("Agg")  # headless
    import matplotlib.pyplot as plt  # noqa: F401
    return True
  except Exception:
    return False


def _save_baseline_policy_plots(df, assets_dir: Path) -> List[str]:
  import pandas as pd  # type: ignore
  import matplotlib.pyplot as plt  # type: ignore

  needed = {"policy", "workflows", "ablation_name", "makespan_p50_ms", "makespan_p95_ms", "makespan_p99_ms"}
  missing = [c for c in needed if c not in df.columns]
  if missing:
    raise SystemExit(f"missing columns for plotting: {missing}")

  base = df[df["ablation_name"] == "baseline"].copy()
  if base.empty:
    base = df.copy()

  for c in ("makespan_p50_ms", "makespan_p95_ms", "makespan_p99_ms", "cost_mean"):
    if c in base.columns:
      base[c] = pd.to_numeric(base[c], errors="coerce")

  workflows_vals = sorted([int(x) for x in base["workflows"].dropna().unique().tolist()])
  out_files: List[str] = []

  fig, axes = plt.subplots(1, 3, figsize=(16, 4), sharey=False)
  metrics = [("makespan_p50_ms", "Makespan p50 (ms)"),
             ("makespan_p95_ms", "Makespan p95 (ms)"),
             ("makespan_p99_ms", "Makespan p99 (ms)")]

  for ax, (col, title) in zip(axes, metrics):
    rows = []
    for wf in workflows_vals:
      grp = base[base["workflows"] == wf].groupby("policy")[col].mean().reset_index()
      grp["workflows"] = wf
      rows.append(grp)
    if rows:
      plot_df = pd.concat(rows, ignore_index=True)
    else:
      plot_df = base.groupby("policy")[col].mean().reset_index()
      plot_df["workflows"] = "all"

    policies = sorted(plot_df["policy"].astype(str).unique().tolist())
    x = range(len(policies))
    vals = [plot_df[plot_df["policy"] == p][col].mean() for p in policies]
    ax.bar(list(x), vals)
    ax.set_xticks(list(x))
    ax.set_xticklabels(policies, rotation=20, ha="right")
    ax.set_title(title)
    ax.grid(axis="y", alpha=0.25)

  fig.tight_layout()
  out_path = assets_dir / "makespan_by_policy.png"
  fig.savefig(out_path, dpi=160)
  plt.close(fig)
  out_files.append(out_path.name)

  if "cost_mean" in base.columns:
    fig, ax = plt.subplots(1, 1, figsize=(6.5, 4))
    cost = base.groupby("policy")["cost_mean"].mean().reset_index()
    policies = sorted(cost["policy"].astype(str).tolist())
    x = range(len(policies))
    vals = [float(cost[cost["policy"] == p]["cost_mean"].iloc[0]) for p in policies]
    ax.bar(list(x), vals)
    ax.set_xticks(list(x))
    ax.set_xticklabels(policies, rotation=20, ha="right")
    ax.set_title("Cost mean (baseline)")
    ax.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    out_path = assets_dir / "cost_by_policy.png"
    fig.savefig(out_path, dpi=160)
    plt.close(fig)
    out_files.append(out_path.name)

  return out_files


def _save_full_ablation_delta_plot(df, assets_dir: Path) -> Optional[str]:
  import pandas as pd  # type: ignore
  import matplotlib.pyplot as plt  # type: ignore

  if "policy" not in df.columns or "ablation_name" not in df.columns or "makespan_p95_ms" not in df.columns:
    return None

  d = df.copy()
  d["makespan_p95_ms"] = pd.to_numeric(d["makespan_p95_ms"], errors="coerce")
  d = d[d["policy"].astype(str) == "full"].dropna(subset=["makespan_p95_ms"])
  if d.empty:
    return None

  base = d[d["ablation_name"] == "baseline"]
  if base.empty:
    return None

  join_keys = [k for k in ("workflows", "seed") if k in d.columns]
  if join_keys:
    merged = d.merge(
      base[join_keys + ["makespan_p95_ms"]].rename(columns={"makespan_p95_ms": "baseline_p95"}),
      on=join_keys,
      how="left",
    )
    merged["ratio"] = merged["makespan_p95_ms"] / merged["baseline_p95"]
    ratios = merged.groupby("ablation_name")["ratio"].mean().reset_index()
  else:
    means = d.groupby("ablation_name")["makespan_p95_ms"].mean().reset_index()
    baseline_mean = float(base["makespan_p95_ms"].mean())
    means["ratio"] = means["makespan_p95_ms"] / baseline_mean
    ratios = means[["ablation_name", "ratio"]]

  ratios = ratios.sort_values("ratio", ascending=False)
  fig, ax = plt.subplots(1, 1, figsize=(7.5, 4))
  names = ratios["ablation_name"].astype(str).tolist()
  x = range(len(names))
  vals = ratios["ratio"].astype(float).tolist()
  ax.bar(list(x), vals)
  ax.axhline(1.0, color="black", linewidth=1)
  ax.set_xticks(list(x))
  ax.set_xticklabels(names, rotation=20, ha="right")
  ax.set_ylabel("p95 makespan ratio vs baseline (full policy)")
  ax.set_title("Ablation deltas (full)")
  ax.grid(axis="y", alpha=0.25)
  fig.tight_layout()

  out_path = assets_dir / "ablation_deltas_full_p95_ratio.png"
  fig.savefig(out_path, dpi=160)
  plt.close(fig)
  return out_path.name


def _write_index_html(out_path: Path, title: str, images: List[str], table_html: str) -> None:
  imgs = "\n".join(
    f'<div class="card"><img src="assets/{html.escape(img)}" alt="{html.escape(img)}"></div>'
    for img in images
  )
  out_path.parent.mkdir(parents=True, exist_ok=True)
  out_path.write_text(
    f"""<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <title>{html.escape(title)}</title>
  <style>
    body {{ font-family: -apple-system, BlinkMacSystemFont, Segoe UI, Roboto, Helvetica, Arial, sans-serif; margin: 24px; }}
    h1 {{ margin: 0 0 12px 0; }}
    .grid {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 16px; margin: 16px 0 24px; }}
    .card {{ border: 1px solid #e6e6e6; border-radius: 10px; padding: 12px; background: #fff; }}
    img {{ max-width: 100%; height: auto; }}
    table {{ border-collapse: collapse; width: 100%; font-size: 12px; }}
    th, td {{ border: 1px solid #e6e6e6; padding: 6px 8px; text-align: left; }}
    th {{ cursor: pointer; background: #fafafa; position: sticky; top: 0; }}
    .hint {{ color: #666; font-size: 12px; }}
  </style>
</head>
<body>
  <h1>{html.escape(title)}</h1>
  <div class="hint">Tip: click table headers to sort.</div>
  <div class="grid">
    {imgs}
  </div>
  {table_html}
  <script>
    function sortTable(table, colIndex) {{
      const tbody = table.tBodies[0];
      const rows = Array.from(tbody.rows);
      const asc = table.getAttribute("data-sort-dir") !== "asc";
      rows.sort((a, b) => {{
        const av = a.cells[colIndex]?.innerText ?? "";
        const bv = b.cells[colIndex]?.innerText ?? "";
        const an = parseFloat(av), bn = parseFloat(bv);
        const bothNum = !isNaN(an) && !isNaN(bn);
        if (bothNum) return asc ? (an - bn) : (bn - an);
        return asc ? av.localeCompare(bv) : bv.localeCompare(av);
      }});
      for (const r of rows) tbody.appendChild(r);
      table.setAttribute("data-sort-dir", asc ? "asc" : "desc");
    }}
    for (const th of document.querySelectorAll("table thead th")) {{
      th.addEventListener("click", () => {{
        const table = th.closest("table");
        sortTable(table, th.cellIndex);
      }});
    }}
  </script>
</body>
</html>
""",
    encoding="utf-8",
  )


def _rows_to_html_table(headers: list[str], rows: list[dict[str, str]]) -> str:
  ths = "".join(f"<th>{html.escape(h)}</th>" for h in headers)
  body_rows = []
  for r in rows:
    tds = "".join(f"<td>{html.escape(r.get(h, ''))}</td>" for h in headers)
    body_rows.append(f"<tr>{tds}</tr>")
  tbody = "\n".join(body_rows)
  return f"<table><thead><tr>{ths}</tr></thead><tbody>{tbody}</tbody></table>"


def main(argv: Optional[List[str]] = None) -> int:
  p = argparse.ArgumentParser(description="Generate a static HTML report for a bench sweep.")
  p.add_argument("--exp_dir", required=True, help="Experiment directory, e.g. bench_runs/my_exp")
  p.add_argument(
    "--runs_csv",
    default=None,
    help="Aggregate CSV path (defaults to <exp_dir>/aggregate/runs.csv).",
  )
  p.add_argument(
    "--out_dir",
    default=None,
    help="Report output dir (defaults to <exp_dir>/report).",
  )
  args = p.parse_args(argv)

  exp_dir = Path(args.exp_dir).expanduser()
  if not exp_dir.is_absolute():
    exp_dir = (REPO_ROOT / exp_dir).resolve()

  runs_csv = Path(args.runs_csv).expanduser() if args.runs_csv else (exp_dir / "aggregate" / "runs.csv")
  if not runs_csv.is_absolute():
    runs_csv = (REPO_ROOT / runs_csv).resolve()

  out_dir = Path(args.out_dir).expanduser() if args.out_dir else (exp_dir / "report")
  if not out_dir.is_absolute():
    out_dir = (REPO_ROOT / out_dir).resolve()
  assets_dir = out_dir / "assets"
  assets_dir.mkdir(parents=True, exist_ok=True)

  headers, rows = _read_csv_rows(runs_csv)
  df = _try_load_pandas_df(runs_csv)
  have_mpl = _try_enable_matplotlib()

  images: List[str] = []
  notes: List[str] = []
  if df is not None and have_mpl:
    images += _save_baseline_policy_plots(df, assets_dir)
    ab_img = _save_full_ablation_delta_plot(df, assets_dir)
    if ab_img:
      images.append(ab_img)
  else:
    if df is None:
      notes.append("Install pandas to enable plots (pip install -r bench/requirements.txt).")
    if not have_mpl:
      notes.append("Install matplotlib to enable plots (pip install -r bench/requirements.txt).")

  table_html = _rows_to_html_table(headers, rows)
  if notes:
    note_html = "<div class='card'><b>Notes</b><ul>" + "".join(
      f"<li>{html.escape(n)}</li>" for n in notes
    ) + "</ul></div>"
    table_html = note_html + "\n" + table_html

  _write_index_html(out_dir / "index.html", f"Bench report: {exp_dir.name}", images, table_html)
  print(f"wrote: {out_dir / 'index.html'}")
  return 0


if __name__ == "__main__":
  raise SystemExit(main())
