#!/usr/bin/env python3
# Copyright: (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
"""Analytical end-to-end model: 15_hisparse_kv_gather measurements -> HiSparse decode step.

Folds the measured gather cost curves (or, without a CSV, 14's calibrated device-RDMA
cost model) into a HiSparse decode step and reports swap-in time, step time and TPS
for three retrieval paths:

  fabric-per-layer : every layer of the group gathers its own miss*k slices of s bytes
                     (today's sglang hisparse host-pool semantics, moved onto fabric)
  fabric-chunk     : one gather of miss*k segments of f*s bytes per layer group
                     (the disclosure's GBT affine chunk layout)
  local-pcie       : current NPU deployment - entry-major contiguous group bursts
                     from LOCAL pinned host memory over PCIe

Model (per decode step, per anchor layer group):
  T_group = launches * launch_us + per_seg_us * segs(miss*k)
where per_seg_us comes from the measured row (us_per_round minus its launch share,
divided by segs_per_round) and segs are scaled linearly by the miss rate. The
baseline step time anchors the compute side; swap-in is added as exposed time
(--exposure scales it, 1.0 = fully on the critical path, conservative).

Baseline anchor (defaults): sglang 2026-08-28 NPU log, GLM-5.2-w4a8 on 8x910C,
bs=1 @128K ctx, 18.07 tok/s -> 55.34 ms/step. NOTE: that run had the full KV on
device (hisparse disabled), so it represents the attention/compute portion; the
swap-in term is what hisparse adds on top.

Outputs: the block-size utilization curve (PPT figure-6 backfill), the per-miss-rate
path table, and sensitivity over f. All tables are plain markdown; --out saves them.
"""

import argparse
import csv
import os
import sys

# 14's calibrated device-RDMA cost model (2026-10-12, 200G RoCE, N=4, K=32):
# t_write(size) = max(0.36us, size / 23.5GB/s), t_read(size) = max(0.22us, size / 23.5GB/s)
FALLBACK_T_WRITE_FLOOR_US = 0.36
FALLBACK_T_READ_FLOOR_US = 0.22
FALLBACK_WIRE_GBS = 23.5

DEFAULT_MISS = "0.1,0.3,0.5,0.8"
DEFAULT_K = 2048
DEFAULT_F = 4
DEFAULT_S = 1280
DEFAULT_ANCHORS = 21  # GLM-5.2: 78 layers, f=4 IndexShare -> 21 full/anchor layers
DEFAULT_BASELINE_STEP_MS = 55.34  # 18.07 tok/s @ bs=1, 128K ctx (2026-08-28 sglang log)
DEFAULT_LAUNCH_US = 30.0  # one batch submit ~= 0.03ms wall (14's submit measurements)
DEFAULT_PCIE_GBS = 20.0  # effective pinned-host H2D for contiguous group bursts
DEFAULT_LINE_RATE = 25.0


def _load_csv_rows(path, run_id, per_layer_layout):
    """Return {(layout_class, seg_bytes, segs, direction): row} - last row per key.

    layout_class collapses per_layer_fbatch/per_layer_1batch to "per_layer"; the key
    carries seg_bytes+segs so different f values of the same layout stay distinct.
    """
    if not path or not os.path.exists(path):
        return {}
    picked = {}
    with open(path, newline="") as fh:
        for row in csv.DictReader(fh):
            if run_id and row.get("run_id") != run_id:
                continue
            if row.get("variant", "").startswith(("checked", "graph-")):
                continue  # host-checked path is not a deployment candidate
            lay = row["layout"]
            lay_class = "per_layer" if lay.startswith("per_layer") else lay
            if lay.startswith("per_layer") and lay != per_layer_layout:
                continue  # only the chosen per-layer representative feeds the model
            key = (lay_class, int(row["seg_bytes"]), int(row["segs_per_round"]), row["direction"])
            picked[key] = row
    return picked


def _measured_us_per_round(rows, layout_want, f, s, direction, k):
    """us_per_round + segs measured for (layout, f, s, direction); None if absent."""
    seg_bytes = f * s if layout_want == "chunk" else s  # CSV per-layer rows key on s
    segs = k if layout_want == "chunk" else f * k
    row = rows.get((layout_want, seg_bytes, segs, direction))
    if row is None:
        return None
    return float(row["us_per_round"]), segs


def _seg_us_fallback(seg_bytes, direction):
    floor = FALLBACK_T_WRITE_FLOOR_US if direction == "write" else FALLBACK_T_READ_FLOOR_US
    return max(floor, seg_bytes / (FALLBACK_WIRE_GBS * 1e9) * 1e6)


def _gather_us(us_per_round, segs_measured, segs_needed, launches, launch_us):
    per_seg = max(0.0, us_per_round - launches * launch_us) / segs_measured
    return launches * launch_us + per_seg * segs_needed


def _path_times(args, rows, miss):
    """(label -> swap-in us per decode step) for one miss rate."""
    k, f, s = args.k, args.f, args.s
    anchors = args.anchors
    out = {}

    # fabric-per-layer: f batch submits, f*miss*k segments of s bytes
    m = _measured_us_per_round(rows, "per_layer", f, s, "read", k)
    if m is not None:
        us_group = _gather_us(m[0], m[1], f * int(miss * k), f, args.launch_us)
    else:
        per_seg = _seg_us_fallback(s, "read")
        us_group = f * args.launch_us + per_seg * f * miss * k
    out["fabric-per-layer"] = anchors * us_group

    # fabric-chunk: one submit, miss*k segments of f*s bytes
    m = _measured_us_per_round(rows, "chunk", f, s, "read", k)
    if m is not None:
        us_group = _gather_us(m[0], m[1], int(miss * k), 1, args.launch_us)
    else:
        per_seg = _seg_us_fallback(f * s, "read")
        us_group = args.launch_us + per_seg * miss * k
    out["fabric-chunk"] = anchors * us_group

    # local-pcie: contiguous group bursts, miss*k*f*s bytes over effective PCIe BW
    bytes_per_step = anchors * miss * k * f * s
    out["local-pcie"] = args.launch_us + bytes_per_step / (args.pcie_gbs * 1e9) * 1e6
    return out


def _fmt_table(header, rows):
    lines = ["| " + " | ".join(header) + " |", "|" + "|".join(["---"] * len(header)) + "|"]
    for r in rows:
        lines.append("| " + " | ".join(str(x) for x in r) + " |")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--csv",
        default=None,
        help="gather_results.csv from 15_hisparse_kv_gather (omit to use 14's calibrated cost model)",
    )
    parser.add_argument("--run-id", default=None, help="use only rows of this run_id")
    parser.add_argument("--k", type=int, default=DEFAULT_K, help=f"top-k (default {DEFAULT_K})")
    parser.add_argument(
        "--f", type=int, default=DEFAULT_F, help=f"layers per group (default {DEFAULT_F}; GLM-5.2 IndexShare)"
    )
    parser.add_argument("--s", type=int, default=DEFAULT_S, help=f"slice bytes (default {DEFAULT_S} = bf16 MLA 576+64)")
    parser.add_argument(
        "--anchors",
        type=int,
        default=DEFAULT_ANCHORS,
        help=f"full/anchor layer groups per decode step (default {DEFAULT_ANCHORS})",
    )
    parser.add_argument("--miss", default=DEFAULT_MISS, help=f"swap-in miss rates to sweep (default {DEFAULT_MISS})")
    parser.add_argument(
        "--baseline-step-ms",
        type=float,
        default=DEFAULT_BASELINE_STEP_MS,
        help=f"compute-side decode step anchor (default {DEFAULT_BASELINE_STEP_MS} from the 2026-08-28 sglang NPU log)",
    )
    parser.add_argument("--bs", type=int, default=1, help="decode batch size (default 1)")
    parser.add_argument(
        "--exposure",
        type=float,
        default=1.0,
        help="fraction of swap-in time exposed on the critical path (default 1.0 = fully serialized, conservative)",
    )
    parser.add_argument(
        "--launch-us",
        type=float,
        default=DEFAULT_LAUNCH_US,
        help=f"per batch-submit wall clock (default {DEFAULT_LAUNCH_US})",
    )
    parser.add_argument(
        "--pcie-gbs",
        type=float,
        default=DEFAULT_PCIE_GBS,
        help=f"effective pinned-host H2D bandwidth for local-pcie path (default {DEFAULT_PCIE_GBS})",
    )
    parser.add_argument(
        "--line-rate",
        type=float,
        default=DEFAULT_LINE_RATE,
        help=f"utilization denominator (default {DEFAULT_LINE_RATE})",
    )
    parser.add_argument("--out", default=None, help="also write the report to this file")
    args = parser.parse_args()

    miss_list = sorted({float(t) for t in args.miss.split(",") if t.strip()})
    rows = _load_csv_rows(args.csv, args.run_id, "per_layer_fbatch")
    source = (
        f"CSV {args.csv}" + (f" run {args.run_id}" if args.run_id else "")
        if rows
        else "fallback cost model (14, 2026-10-12 calibration: t=max(0.36/0.22us, size/23.5GB/s))"
    )

    report = []
    report.append("# HiSparse end-to-end projection (e2e_model)")
    report.append("")
    report.append(f"- cost source: {source}")
    report.append(
        f"- model: k={args.k}, f={args.f}, s={args.s}B (chunk {args.f * args.s}B), "
        f"anchors/step={args.anchors}, exposure={args.exposure}"
    )
    report.append(
        f"- baseline: {args.baseline_step_ms:.2f} ms/step "
        f"({args.bs / (args.baseline_step_ms / 1000):.2f} tok/s @ bs={args.bs}); "
        f"local-pcie reference {args.pcie_gbs} GB/s"
    )
    report.append("")

    # ---- table 1: block-size utilization curve (PPT figure-6 backfill) ----
    report.append("## 1. Block-size utilization curve (PPT figure-6 backfill, read)")
    curve = []
    if rows:
        seen = {}
        for (lay, seg_bytes, segs, direction), row in rows.items():
            if direction != "read":
                continue
            seen[(seg_bytes, lay)] = row
        for (seg_bytes, lay), row in sorted(seen.items()):
            curve.append((seg_bytes, row["layout"], float(row["bw_gb_s"]), f"{float(row['util_pct']):.1f}%"))
    else:
        for seg_bytes in (640, 1280, 2560, 5120, 10240, 20480):
            t = _seg_us_fallback(seg_bytes, "read")
            bw = seg_bytes / t / 1e3  # GB/s
            curve.append((seg_bytes, "model", round(bw, 2), f"{bw / args.line_rate * 100:.1f}%"))
    report.append(_fmt_table(["seg bytes", "layout", "BW GB/s (decimal)", "util vs line rate"], curve))
    report.append("")

    # ---- table 2: per-miss-rate path table ----
    report.append("## 2. Decode-step projection by miss rate")
    base_ms = args.baseline_step_ms
    body = []
    for miss in miss_list:
        times = _path_times(args, rows, miss)
        for label, us in times.items():
            step_ms = base_ms + args.exposure * us / 1e3
            tps = args.bs / (step_ms / 1e3)
            body.append((f"{miss:.0%}", label, f"{us / 1e3:.2f}", f"{step_ms:.2f}", f"{tps:.2f}"))
    report.append(_fmt_table(["miss", "path", "swap-in ms/step", "step ms", "tok/s @ bs"], body))
    report.append("")

    # ---- table 3: gain summary at each miss rate ----
    report.append("## 3. Gain summary (fabric-chunk vs alternatives)")
    body = []
    for miss in miss_list:
        t = _path_times(args, rows, miss)
        chunk = t["fabric-chunk"]
        for label in ("fabric-per-layer", "local-pcie"):
            other = t[label]
            gain = other / chunk if chunk > 0 else float("nan")
            step_c = base_ms + args.exposure * chunk / 1e3
            step_o = base_ms + args.exposure * other / 1e3
            tps_c = args.bs / (step_c / 1e3)
            tps_o = args.bs / (step_o / 1e3)
            body.append(
                (
                    f"{miss:.0%}",
                    f"chunk vs {label}",
                    f"{gain:.2f}x",
                    f"{tps_o:.2f} -> {tps_c:.2f} tok/s",
                    f"{(tps_c / tps_o - 1) * 100:+.1f}%",
                )
            )
    report.append(_fmt_table(["miss", "comparison", "swap-in speedup", "TPS", "TPS gain"], body))
    report.append("")

    # ---- PPT claim echo ----
    report.append("## 4. PPT L1 claim check")
    s_row = rows.get(("per_layer", args.s, args.f * args.k, "read"))
    fs_row = rows.get(("chunk", args.f * args.s, args.k, "read"))
    if s_row and fs_row:
        report.append(
            f"- per-layer {args.s}B util {float(s_row['util_pct']):.1f}% "
            f"(claim: <30%); chunk {args.f * args.s}B util "
            f"{float(fs_row['util_pct']):.1f}% (claim: >=60%)"
        )
    else:
        report.append("- no CSV rows for the exact (s, f*s) pair; see table 1 curve")
    report.append("")
    report.append("## Caveats")
    report.append("- Baseline step is a NON-hisparse run (KV on device); swap-in is treated")
    report.append("  as additive exposed time. Real overlap (prefetch streams, multi-request")
    report.append("  batching) lowers the effective exposure -- rerun with --exposure < 1.")
    report.append("- Segment-time linear in miss count assumes the measured pipelining")
    report.append("  (multi-QP, doorbell batching) holds at partial segment counts.")
    report.append("- Miss rate must come from a real hisparse run (coordinator miss stats)")
    report.append("  for absolute numbers; the sweep here is a sensitivity envelope.")

    text = "\n".join(report)
    print(text)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write(text + "\n")
        print(f"\n[e2e] report saved to {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
