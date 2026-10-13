#!/usr/bin/env python3
# Copyright: (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
"""HiSparse KV gather A/B benchmark: per-layer layout vs layer-group chunk layout.

Motivation (patent disclosure "基于内存语义直接寻址的KV缓存组块读取", L1 experiment):
HiSparse decode keeps only a hot KV buffer on device and swaps in the top-k most
relevant tokens per layer group from a pooled host/fabric store. Two physical
layouts of the same logical KV data give two very different gather shapes at the
transport level:

  Layout A (per-layer, today's sglang hisparse host pool):
      layer l of token t lives at  layer_base[l] + t * s          -> f batches
      (or one interleaved batch) of k segments x s bytes each. s = one MLA slice,
      e.g. 1280B (bf16, 576+64 dims).
  Layout B (layer-group chunk, the disclosure's GBT affine layout):
      token t's f slices are co-located at  GBT + t * (f*s), slice i at +i*s
      -> ONE batch of k segments x f*s bytes each (e.g. 5.1KB at f=4).

Both layouts move the SAME k*f*s bytes per gather round; the A/B comparison isolates
"retrieval unit / contiguity" from raw block-size effects. Top-k indices are random
over a simulated context window (use --topk-pattern contiguous to separate the
randomness effect). Expected validation on 200G RoCE (25 GB/s line rate): the
s-byte per-layer regime stays well under ~30% utilization while the f*s chunk
regime reaches >=60% (the disclosure's claim).

Topology is 14's (FAR daemon contributes one DRAM slot; this client registers HBM
tensors and drives device-scheduled RDMA batches). Per (f, s) config:

  seed two FAR regions (layout-A arrangement + layout-B arrangement, same logical
  content) -> checked-path warmup gather + verify -> per layout: timed READ phase
  (swap-in shape) and timed WRITE phase (eager-backup shape) with round-trip verify
  -> CSV row. --graph captures the round submits into an NPUGraph and replays them
  (decode-graph embedding shape).

Output: 14-style per-config lines plus a CSV (log/gather_results.csv) and a final
A/B summary with the PPT L1 utilization check.
"""

import argparse
import csv
import os
import socket
import sys
import time

import memfabric_hybrid as mf
import numpy as np
from memfabric_hybrid import ralloc

DEFAULT_WORLD = 512  # declared world capacity, actual members join dynamically
NIC_PORT_BASE = 10005  # data-plane nic port base (set_nic); NOT the control rpc port
RPC_PORT_BASE = 11100  # control rpc port = base + rankId (smem_ralloc_def.h default)
DEFAULT_K = 2048  # top-k sparse indices per layer group (GLM-5.2 index_topk)
DEFAULT_F_LIST = "1,2,4,8"  # layers per group (f=4 matches GLM-5.2 IndexShare)
DEFAULT_S_LIST = "1280"  # bytes per (token, layer) MLA slice; bf16 576+64 dims
DEFAULT_CTX = 32768  # simulated context window (tokens) -> address spread
DEFAULT_ROUND_BYTES = "64M"  # target one-way volume per (config, layout, direction)
MIN_ROUNDS = 3
DEFAULT_REAL_POOL_SIZE = "2G"  # FAR slot bytes (must cover 2 * ctx * f_max * s_max)
DEFAULT_MAX_POOL_SIZE = "4G"  # pool DRAM window declared to the FAR placement master
META_HBM_WINDOW = 1 << 30  # minimal GB-aligned hbm window: device meta window only
DEFAULT_LINE_RATE = 25.0  # 200G RoCE one-way line rate in decimal GB/s (utilization %)
DEFAULT_REPLAYS = 4
DEFAULT_SEED = 20261013
LAYOUTS_ALL = ("chunk", "per_layer_fbatch", "per_layer_1batch")
VARIANTS = ("v3", "v2", "checked")
SEED_CHUNK = 8 << 20
PATTERN_MAGIC = 2654435761  # Knuth multiplicative hash constant (unique-ish word pattern)
GIB = 1 << 30
EXTEND_RETRY_SEC = 5
EXTEND_TIMEOUT_SEC = 300

DATA_OP = ralloc.RallocDataOpType.DEVICE_RDMA | ralloc.RallocDataOpType.DEVICE_SCHEDULE
MEM_TYPE = ralloc.RallocMemType.HOST

CSV_COLUMNS = [
    "run_id",
    "layout",
    "direction",
    "variant",
    "pattern",
    "k",
    "f",
    "s_bytes",
    "seg_bytes",
    "segs_per_round",
    "batches_per_round",
    "bytes_per_round",
    "rounds",
    "bw_gb_s",
    "bw_gib_s_14conv",
    "util_pct",
    "us_per_round",
    "us_per_seg",
    "submit_ms",
    "line_rate_gb_s",
]

_term_fd = None


def _log(msg):
    print(msg, flush=True)
    if _term_fd is not None:
        os.write(_term_fd, (msg + "\n").encode("utf-8", "replace"))


def _parse_size(tok):
    tok = tok.strip().upper()
    mult = 1
    if tok.endswith("K"):
        mult, tok = 1024, tok[:-1]
    elif tok.endswith("M"):
        mult, tok = 1 << 20, tok[:-1]
    elif tok.endswith("G"):
        mult, tok = 1 << 30, tok[:-1]
    n = int(tok)
    if n <= 0:
        raise ValueError(f"bad size token: {tok}")
    return n * mult


def _aligned_npu_tensor(nbytes, device):
    """4K-aligned int32 view of nbytes (multiple of 4) on `device`."""
    import torch

    buf = torch.empty((nbytes + 4096) // 4, dtype=torch.int32, device=device)
    off = ((-buf.data_ptr()) % 4096) // 4
    return buf[off : off + nbytes // 4]


class BatchPlan:
    """One device_copy batch submit plan; keeps staged descriptor tensors alive.

    variant=v3  : three SoA arrays staged to HBM, peer/direction as call scalars
                  (uniform contract: one peer, one direction, src/dst each in one mkey)
    variant=v2  : five SoA arrays staged to HBM (routed fast path)
    variant=checked: plain Python address lists through device_copy_batch
    """

    def __init__(self, handle, variant, srcs, dsts, sizes, far_rank, is_write):
        self.handle = handle
        self.variant = variant
        self.n = len(sizes)
        self.keep = []
        if variant == "v3":
            dev, src_va, dst_va, size_va = _stage_batch_desc_v3(srcs, dsts, sizes)
            self.keep.append(dev)
            self.src_va, self.dst_va, self.size_va = src_va, dst_va, size_va
            self.rank_va = self.write_va = None
            self.peer, self.iw = far_rank, is_write
        elif variant == "v2":
            dev, src_va, dst_va, size_va, rank_va, write_va = _stage_batch_desc(
                srcs, dsts, sizes, [far_rank] * self.n, [is_write] * self.n
            )
            self.keep.append(dev)
            self.src_va, self.dst_va, self.size_va = src_va, dst_va, size_va
            self.rank_va, self.write_va = rank_va, write_va
        else:
            self.srcs, self.dsts, self.sizes = list(srcs), list(dsts), list(sizes)

    def submit(self, stream):
        if self.variant == "v3":
            return self.handle.device_copy_batch_v3(
                self.src_va, self.dst_va, self.size_va, self.peer, self.iw, self.n, stream
            )
        if self.variant == "v2":
            return self.handle.device_copy_batch_v2(
                self.src_va, self.dst_va, self.size_va, self.rank_va, self.write_va, self.n, stream
            )
        return self.handle.device_copy_batch(self.srcs, self.dsts, self.sizes, stream)


def _stage_batch_desc(srcs, dsts, sizes, ranks, writes):
    """Pack the five SoA descriptor arrays into ONE contiguous HBM buffer (32B/seg)."""
    import torch

    n = len(srcs)
    host = np.empty(n * 32, dtype=np.uint8)
    host[0 : n * 8] = np.asarray(srcs, dtype=np.uint64).view(np.uint8)
    host[n * 8 : n * 16] = np.asarray(dsts, dtype=np.uint64).view(np.uint8)
    host[n * 16 : n * 24] = np.asarray(sizes, dtype=np.uint64).view(np.uint8)
    host[n * 24 : n * 28] = np.asarray(ranks, dtype=np.uint32).view(np.uint8)
    host[n * 28 : n * 32] = np.asarray(writes, dtype=np.uint32).view(np.uint8)
    dev = torch.from_numpy(host).to("npu")
    base = dev.data_ptr()
    return dev, base, base + n * 8, base + n * 16, base + n * 24, base + n * 28


def _stage_batch_desc_v3(srcs, dsts, sizes):
    """Uniform (v3) staging: three SoA arrays, 24B/seg."""
    import torch

    n = len(srcs)
    host = np.empty(n * 24, dtype=np.uint8)
    host[0 : n * 8] = np.asarray(srcs, dtype=np.uint64).view(np.uint8)
    host[n * 8 : n * 16] = np.asarray(dsts, dtype=np.uint64).view(np.uint8)
    host[n * 16 : n * 24] = np.asarray(sizes, dtype=np.uint64).view(np.uint8)
    dev = torch.from_numpy(host).to("npu")
    base = dev.data_ptr()
    return dev, base, base + n * 8, base + n * 16


def _wait_tcp(url, timeout_sec):
    host, port = url.split("://", 1).rsplit(":", 1)
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        with socket.socket() as s:
            if s.connect_ex((host, int(port))) == 0:
                return
        time.sleep(0.5)
    raise RuntimeError(f"endpoint not reachable within {timeout_sec}s: {url} — is the daemon up?")


def _gen_topk(pattern, ctx, k, seed):
    if pattern == "contiguous":
        idx = np.arange(k, dtype=np.int64)
    else:
        rng = np.random.default_rng(seed)
        idx = np.sort(rng.choice(ctx, size=k, replace=False))
    return idx


def _layout_batches(layout, base_a, base_b, dst_addr, topk, f, s, ctx, k, handle, variant, far_rank, is_write):
    """Return a list of BatchPlans whose sequential submission forms one gather round.

    Address roles follow the data-plane convention (see 14's write phase): a READ
    (swap-in) gathers FAR -> local HBM rows; a WRITE (eager backup) pushes local HBM
    rows -> FAR. Both directions move the same per-segment (far_off, hbm_off) pairs.
    """
    fs = f * s

    def orient(far_addrs, hbm_addrs, sizes):
        if is_write:
            return BatchPlan(handle, variant, hbm_addrs, far_addrs, sizes, far_rank, is_write)
        return BatchPlan(handle, variant, far_addrs, hbm_addrs, sizes, far_rank, is_write)

    if layout == "chunk":
        far_addrs = [base_b + int(t) * fs for t in topk]
        hbm_addrs = [dst_addr + j * fs for j in range(k)]
        sizes = [fs] * k
        return [orient(far_addrs, hbm_addrs, sizes)]
    # per-layer: layer l of token t at base_a + l*ctx*s + t*s; dst rows stay token-major
    if layout == "per_layer_fbatch":
        plans = []
        for l in range(f):
            lbase = base_a + l * ctx * s
            far_addrs = [lbase + int(t) * s for t in topk]
            hbm_addrs = [dst_addr + j * fs + l * s for j in range(k)]
            plans.append(orient(far_addrs, hbm_addrs, [s] * k))
        return plans
    if layout == "per_layer_1batch":
        far_addrs, hbm_addrs, sizes = [], [], []
        for l in range(f):
            lbase = base_a + l * ctx * s
            for j, t in enumerate(topk):
                far_addrs.append(lbase + int(t) * s)
                hbm_addrs.append(dst_addr + j * fs + l * s)
                sizes.append(s)
        return [orient(far_addrs, hbm_addrs, sizes)]
    raise RuntimeError(f"unknown layout {layout}")


def _seed_region(handle, src_addr, base, nbytes, stream_ptr):
    off = 0
    while off < nbytes:
        n = min(SEED_CHUNK, nbytes - off)
        assert handle.device_copy(src_addr + off, base + off, n, stream_ptr) == 0, "seed chunk failed"
        off += n


def _submit_round(plans, stream):
    for p in plans:
        assert p.submit(stream) == 0, "batch submit failed"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--store", required=True, help="store url of the FAR daemon, e.g. tcp://10.0.0.1:8588")
    parser.add_argument("--dev", type=int, required=True, help="NPU id this client runs on")
    parser.add_argument("--k", type=int, default=DEFAULT_K, help=f"top-k tokens per layer group (default {DEFAULT_K})")
    parser.add_argument(
        "--f-list", default=DEFAULT_F_LIST, help=f"comma list of layers-per-group f values (default {DEFAULT_F_LIST})"
    )
    parser.add_argument(
        "--s-list",
        default=DEFAULT_S_LIST,
        help=f"comma list of per-(token,layer) slice bytes, 4-byte aligned "
        f"(default {DEFAULT_S_LIST}; 1280 = bf16 MLA 576+64, 640 = KV8)",
    )
    parser.add_argument(
        "--ctx-tokens",
        type=int,
        default=DEFAULT_CTX,
        help=f"simulated context window in tokens; top-k indices spread over it (default {DEFAULT_CTX})",
    )
    parser.add_argument(
        "--layouts", default=",".join(LAYOUTS_ALL), help=f"comma list of layouts to time: {LAYOUTS_ALL} (default all)"
    )
    parser.add_argument(
        "--topk-pattern",
        choices=("random", "contiguous"),
        default="random",
        help="random = real top-k gather shape; contiguous isolates the randomness effect (default random)",
    )
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED, help=f"top-k RNG seed (default {DEFAULT_SEED})")
    parser.add_argument(
        "--rounds",
        type=int,
        default=0,
        help=f"gather rounds per timed phase; 0 = auto from --round-bytes (>= {MIN_ROUNDS})",
    )
    parser.add_argument(
        "--round-bytes",
        default=DEFAULT_ROUND_BYTES,
        help=f"target one-way volume per (config, layout, direction) when "
        f"--rounds 0 (K/M/G suffix, default {DEFAULT_ROUND_BYTES})",
    )
    parser.add_argument(
        "--variant",
        choices=VARIANTS,
        default="v3",
        help="device batch path: v3 = uniform 3-array DVA (default), "
        "v2 = routed 5-array DVA, checked = host-checked 64-seg launches",
    )
    parser.add_argument(
        "--directions",
        default="read,write",
        help="timed directions: read = swap-in (RH2D), write = eager backup (D2RH) (default read,write)",
    )
    parser.add_argument(
        "--graph",
        action="store_true",
        help="capture the round submits into ONE NPUGraph per phase and "
        "replay --replays times (decode-graph embedding shape)",
    )
    parser.add_argument(
        "--replays",
        type=int,
        default=DEFAULT_REPLAYS,
        help=f"graph replays per timed phase in graph mode (default {DEFAULT_REPLAYS})",
    )
    parser.add_argument(
        "--real-pool-size",
        default=DEFAULT_REAL_POOL_SIZE,
        help=f"FAR slot bytes, must cover 2 * ctx * max(f) * max(s) (K/M/G suffix, default {DEFAULT_REAL_POOL_SIZE})",
    )
    parser.add_argument(
        "--max-pool-size",
        default=DEFAULT_MAX_POOL_SIZE,
        help=f"pool DRAM window declared to the FAR placement master, must be "
        f"GB aligned (K/M/G suffix, default {DEFAULT_MAX_POOL_SIZE})",
    )
    parser.add_argument(
        "--line-rate",
        type=float,
        default=DEFAULT_LINE_RATE,
        help=f"one-way line rate in decimal GB/s for utilization %% (default {DEFAULT_LINE_RATE:.1f} = 200G RoCE)",
    )
    parser.add_argument("--csv", default=None, help="result CSV path (default <run-dir>/gather_results.csv)")
    parser.add_argument(
        "--world", type=int, default=DEFAULT_WORLD, help=f"declared world capacity (default {DEFAULT_WORLD})"
    )
    parser.add_argument(
        "--rpc-port-base",
        type=int,
        default=RPC_PORT_BASE,
        help=f"control rpc port base (default {RPC_PORT_BASE}); must match the daemon",
    )
    parser.add_argument(
        "--enable-56bits-gva",
        action="store_true",
        help="create the pool with 56-bit GVA (slot GVA stays a device-copy endpoint only, never a CPU pointer)",
    )
    parser.add_argument("--run-dir", default=None, help="client log dir (default ./log)")
    args = parser.parse_args()

    f_list = sorted({int(t) for t in args.f_list.split(",") if t.strip() != ""})
    s_list = sorted({int(t) for t in args.s_list.split(",") if t.strip() != ""})
    layouts = [t.strip() for t in args.layouts.split(",") if t.strip() != ""]
    directions = [t.strip() for t in args.directions.split(",") if t.strip() != ""]
    if not f_list or not s_list or not layouts or not directions:
        raise RuntimeError("empty --f-list / --s-list / --layouts / --directions")
    for bad in layouts:
        if bad not in LAYOUTS_ALL:
            raise RuntimeError(f"unknown layout {bad}; valid: {LAYOUTS_ALL}")
    for d in directions:
        if d not in ("read", "write"):
            raise RuntimeError(f"unknown direction {d}")
    for s in s_list:
        if s % 4 != 0:
            raise RuntimeError(f"s {s} must be 4-byte aligned (int32 verify pattern)")
    if args.k < 1 or args.k > args.ctx_tokens:
        raise RuntimeError(f"--k ({args.k}) must be in [1, --ctx-tokens ({args.ctx_tokens})]")
    if args.replays < 1:
        raise RuntimeError(f"--replays ({args.replays}) must be >= 1")

    region_max = args.ctx_tokens * max(f_list) * max(s_list)
    remote_bytes = _parse_size(args.real_pool_size)
    max_pool_bytes = _parse_size(args.max_pool_size)
    round_target = _parse_size(args.round_bytes)
    if 2 * region_max > remote_bytes:
        raise RuntimeError(
            f"FAR slot too small: 2 layouts x ctx({args.ctx_tokens}) x max_f({max(f_list)}) x "
            f"max_s({max(s_list)}) = {2 * region_max} B > --real-pool-size ({remote_bytes} B); "
            f"raise --real-pool-size or shrink the sweep"
        )
    if remote_bytes > max_pool_bytes:
        raise RuntimeError(f"--real-pool-size ({remote_bytes}) exceeds --max-pool-size ({max_pool_bytes})")
    if max_pool_bytes % GIB != 0:
        raise RuntimeError(f"--max-pool-size ({max_pool_bytes}) must be GB aligned (VMM segment rule)")

    global _term_fd
    run_dir = os.path.abspath(args.run_dir or "./log")
    os.makedirs(run_dir, exist_ok=True)
    log = open(os.path.join(run_dir, f"near_dev{args.dev}.log"), "w")
    _term_fd = os.dup(1)
    os.dup2(log.fileno(), 1)
    os.dup2(log.fileno(), 2)

    run_id = time.strftime("%Y%m%d_%H%M%S")
    csv_path = os.path.abspath(args.csv or os.path.join(run_dir, "gather_results.csv"))
    csv_new = not os.path.exists(csv_path)

    _log(
        f"[client] run dir: {run_dir}, store: {args.store}, world: {args.world}, dev: {args.dev}, "
        f"56bits_gva: {args.enable_56bits_gva}"
    )
    _log(
        f"[client] k={args.k}, f_list={f_list}, s_list={s_list}, ctx={args.ctx_tokens}, "
        f"layouts={layouts}, pattern={args.topk_pattern!r}, variant={args.variant}, "
        f"graph={args.graph}, replays={args.replays}"
    )
    _log(
        f"[client] env: MF_QPS_PER_PEER={os.environ.get('MF_QPS_PER_PEER', '1')}, "
        f"MF_DVA_DB_BATCH={os.environ.get('MF_DVA_DB_BATCH', '32')}, "
        f"line_rate={args.line_rate} GB/s, csv={csv_path}"
    )

    topk = _gen_topk(args.topk_pattern, args.ctx_tokens, args.k, args.seed)

    _wait_tcp(args.store, 30)
    dev = args.dev

    mf.set_log_level(1)
    assert mf.initialize() == 0, "mf.initialize failed"
    ralloc_inited = False
    try:
        import torch
        import torch_npu  # noqa: F401 — registers the npu backend and torch.npu.*

        cfg = ralloc.RallocConfig()
        cfg.auto_ranking = True
        cfg.role = ralloc.RallocRole.NEAR
        cfg.start_store = False
        cfg.dynamic_world_size = True
        cfg.rpc_port_base = args.rpc_port_base
        cfg.set_nic(f"tcp://{socket.gethostbyname(socket.gethostname())}:{NIC_PORT_BASE}")
        assert ralloc.initialize(args.store, args.world, dev, cfg) == 0, "ralloc.initialize failed"
        ralloc_inited = True
        rank = ralloc.get_rank_id()

        handle = ralloc.create(
            id=0,
            max_dram_size=max_pool_bytes,
            max_hbm_size=META_HBM_WINDOW,
            data_op_type=DATA_OP,
            enable_56bits_gva=args.enable_56bits_gva,
        )
        _log(
            f"[client rank {rank}] device-scheduled pool created (DEVICE_RDMA | DEVICE_SCHEDULE, "
            f"56bits_gva={args.enable_56bits_gva})"
        )

        deadline = time.time() + EXTEND_TIMEOUT_SEC
        while True:
            ret, info = handle.extend_remote_mem(MEM_TYPE, remote_bytes)
            if ret == 0 and info.get("gva"):
                break
            if time.time() > deadline:
                raise RuntimeError(f"no FAR candidate within {EXTEND_TIMEOUT_SEC}s: {ret} {info}")
            time.sleep(EXTEND_RETRY_SEC)
        gva = info["gva"]
        far_rank = info["rank_id"]
        mf.get_and_clear_last_err_msg()
        base_a = gva  # per-layer arrangement region
        base_b = gva + region_max  # chunk (GBT) arrangement region
        _log(
            f"[client rank {rank}] remote block from FAR rank {far_rank} (gva=0x{gva:x}, npu {dev}); "
            f"layout regions: A=0x{base_a:x} (+{region_max}B per-layer), B=0x{base_b:x} (+{region_max}B chunk)"
        )

        device = f"npu:{dev}"
        side = torch.npu.Stream()
        csv_rows = []  # full-run rows (summary only); CSV file is flushed per config
        results = {}  # (f, s, layout, direction) -> row dict, for the A/B summary

        for f in f_list:
            for s in s_list:
                cfg_rows = []
                fs = f * s
                region_bytes = args.ctx_tokens * fs
                words_per_row = fs // 4

                # logical KV image: unique int32 word pattern, token-major rows of f*s bytes
                rows = torch.arange(args.ctx_tokens, dtype=torch.int64, device=device).unsqueeze(1)
                cols = torch.arange(words_per_row, dtype=torch.int64, device=device).unsqueeze(0)
                logical = ((rows * PATTERN_MAGIC + cols) % 2147483647).to(torch.int32)
                staging_b = logical  # token-major (chunk)
                staging_a = logical.view(args.ctx_tokens, f, s // 4).permute(1, 0, 2).contiguous()
                topk_t = torch.from_numpy(topk).to(device)
                expected = logical.index_select(0, topk_t)  # (k, words_per_row)
                w_pattern = expected + 1  # write-phase payload

                dst = _aligned_npu_tensor(args.k * fs, device)
                dst_addr = dst.data_ptr()
                sa_addr, sb_addr = staging_a.data_ptr(), staging_b.data_ptr()
                torch.npu.synchronize()
                assert handle.register(dst_addr, args.k * fs) == 0, "register dst failed"
                assert handle.register(sa_addr, region_bytes) == 0, "register staging A failed"
                assert handle.register(sb_addr, region_bytes) == 0, "register staging B failed"

                side.wait_stream(torch.npu.current_stream())
                with torch.npu.stream(side):
                    stream_ptr = torch.npu.current_stream().npu_stream
                    _seed_region(handle, sb_addr, base_b, region_bytes, stream_ptr)
                    _seed_region(handle, sa_addr, base_a, region_bytes, stream_ptr)
                torch.npu.synchronize()
                _log(f"[client] f={f} s={s}: regions seeded ({region_bytes} B per layout, ctx={args.ctx_tokens})")

                # correctness gate on the checked path for BOTH layouts (also dlopens the
                # kernel library outside any graph)
                checked_plans = {
                    (lay, d): _layout_batches(
                        lay,
                        base_a,
                        base_b,
                        dst_addr,
                        topk,
                        f,
                        s,
                        args.ctx_tokens,
                        args.k,
                        handle,
                        "checked",
                        far_rank,
                        1 if d == "write" else 0,
                    )
                    for lay in LAYOUTS_ALL
                    for d in ("read", "write")
                }
                for lay in LAYOUTS_ALL:
                    dst.zero_()
                    torch.npu.synchronize()
                    _submit_round(checked_plans[(lay, "read")], 0)
                    torch.npu.synchronize()
                    assert torch.equal(dst.view(args.k, words_per_row), expected), (
                        f"{lay} read layout/addressing mismatch at f={f} s={s}"
                    )
                _log(f"[client] f={f} s={s}: warmup gather verify OK (all layouts, checked path)")

                # Timed phases. Direction order is fixed read-then-write: a write
                # round scatters the W pattern into the FAR region and would corrupt
                # the seed for any later read phase, so all layouts read first.
                for d in [x for x in ("read", "write") if x in directions]:
                    for lay in layouts:
                        segs = args.k if lay == "chunk" else f * args.k
                        batches = 1 if lay != "per_layer_fbatch" else f
                        bytes_per_round = segs * (fs if lay == "chunk" else s)
                        rounds = args.rounds if args.rounds > 0 else max(MIN_ROUNDS, round_target // bytes_per_round)
                        is_write = 1 if d == "write" else 0
                        plans = _layout_batches(
                            lay,
                            base_a,
                            base_b,
                            dst_addr,
                            topk,
                            f,
                            s,
                            args.ctx_tokens,
                            args.k,
                            handle,
                            args.variant,
                            far_rank,
                            is_write,
                        )
                        # one un-timed round on the timed path (warms the exact variant)
                        if d == "read":
                            dst.zero_()
                        else:
                            dst.copy_(w_pattern.view(-1))
                        torch.npu.synchronize()
                        _submit_round(plans, 0)
                        torch.npu.synchronize()

                        if args.graph:
                            graph = torch.npu.NPUGraph()
                            with torch.npu.stream(side):
                                gstream = torch.npu.current_stream().npu_stream
                                graph.capture_begin()
                                _submit_round(plans, gstream)
                                graph.capture_end()
                            torch.npu.synchronize()
                            if d == "read":
                                dst.zero_()
                                torch.npu.synchronize()
                            t0 = time.perf_counter()
                            for _ in range(args.replays):
                                graph.replay()
                            torch.npu.synchronize()
                            t_total = time.perf_counter() - t0
                            t_submit = 0.0
                            moved = args.replays * bytes_per_round
                            timed_rounds = args.replays
                            del graph
                        else:
                            if d == "read":
                                dst.zero_()
                                torch.npu.synchronize()
                            t0 = time.perf_counter()
                            for _ in range(rounds):
                                _submit_round(plans, 0)
                            t_submit = time.perf_counter() - t0
                            torch.npu.synchronize()
                            t_total = time.perf_counter() - t0
                            moved = rounds * bytes_per_round
                            timed_rounds = rounds

                        # round-trip verification on the checked path
                        if d == "read":
                            assert torch.equal(dst.view(args.k, words_per_row), expected), (
                                f"{lay} read verify mismatch at f={f} s={s}"
                            )
                            verify_note = "verify OK"
                        else:
                            dst.zero_()
                            torch.npu.synchronize()
                            _submit_round(checked_plans[(lay, "read")], 0)
                            torch.npu.synchronize()
                            assert torch.equal(dst.view(args.k, words_per_row), w_pattern), (
                                f"{lay} write round-trip mismatch at f={f} s={s}"
                            )
                            verify_note = "round-trip OK"

                        bw_gbs = moved / t_total / 1e9
                        row = {
                            "run_id": run_id,
                            "layout": lay,
                            "direction": d,
                            "variant": args.variant if not args.graph else f"graph-{args.variant}",
                            "pattern": args.topk_pattern,
                            "k": args.k,
                            "f": f,
                            "s_bytes": s,
                            "seg_bytes": fs if lay == "chunk" else s,
                            "segs_per_round": segs,
                            "batches_per_round": batches,
                            "bytes_per_round": bytes_per_round,
                            "rounds": timed_rounds,
                            "bw_gb_s": f"{bw_gbs:.2f}",
                            "bw_gib_s_14conv": f"{moved / t_total / GIB:.2f}",
                            "util_pct": f"{bw_gbs / args.line_rate * 100:.1f}",
                            "us_per_round": f"{t_total / timed_rounds * 1e6:.2f}",
                            "us_per_seg": f"{t_total / timed_rounds / segs * 1e6:.3f}",
                            "submit_ms": f"{t_submit * 1e3:.2f}",
                            "line_rate_gb_s": f"{args.line_rate:.1f}",
                        }
                        csv_rows.append(row)
                        cfg_rows.append(row)
                        results[(f, s, lay, d)] = row
                        submit_note = "" if args.graph else f", submit {t_submit * 1e3:.2f} ms"
                        _log(
                            f"[client] f={f} s={s} {lay}: {segs} segs x "
                            f"{row['seg_bytes']}B ({batches} batch/round) x {timed_rounds} rounds, "
                            f"{d} {bw_gbs:.2f} GB/s ({row['util_pct']}%), "
                            f"{row['us_per_round']} us/gather "
                            f"({row['us_per_seg']} us/seg){submit_note} [{verify_note}]"
                        )

                assert handle.unregister(dst_addr) == 0, "unregister dst failed"
                assert handle.unregister(sa_addr) == 0, "unregister staging A failed"
                assert handle.unregister(sb_addr) == 0, "unregister staging B failed"
                del dst, staging_a, staging_b, logical, expected, w_pattern, topk_t
                del checked_plans
                # flush this config's rows so partial runs survive later failures
                with open(csv_path, "a", newline="") as fh:
                    writer = csv.DictWriter(fh, fieldnames=CSV_COLUMNS)
                    if csv_new:
                        writer.writeheader()
                        csv_new = False
                    writer.writerows(cfg_rows)
                _log(f"[client] f={f} s={s}: {len(cfg_rows)} rows flushed to {csv_path}")

        # ---- summary: A/B comparison + PPT L1 utilization check ----
        _log("[client] ==== A/B summary (read = swap-in RH2D) ====")
        for f in f_list:
            for s in s_list:
                a = results.get((f, s, "per_layer_fbatch", "read"))
                b = results.get((f, s, "chunk", "read"))
                if a and b and f > 1:
                    a_bw, b_bw = float(a["bw_gb_s"]), float(b["bw_gb_s"])
                    speedup = f"{b_bw / a_bw:.2f}x" if a_bw > 0 else "n/a"
                    _log(
                        f"[client] f={f} s={s}: per-layer {a['segs_per_round']}x{a['seg_bytes']}B "
                        f"read {a['bw_gb_s']} GB/s ({a['util_pct']}%) vs "
                        f"chunk {b['segs_per_round']}x{b['seg_bytes']}B "
                        f"read {b['bw_gb_s']} GB/s ({b['util_pct']}%) -> {speedup}"
                    )
        # PPT L1 check: chunk row with f*s closest to 5.1KB vs any per-layer row at the
        # same s (per-layer seg size is always s, independent of f)
        chunk_best = None
        for (f, s, lay, d), row in results.items():
            if d == "read" and lay == "chunk":
                key = abs(f * s - 5120)
                if chunk_best is None or key < chunk_best[0]:
                    chunk_best = (key, f, s, row)
        if chunk_best is not None:
            _, cf, cs, chunk_row = chunk_best
            per_row = None
            for (f, s, lay, d), row in results.items():
                better = per_row is None or (lay, f) < (per_row[0], per_row[1])
                if d == "read" and s == cs and lay.startswith("per_layer") and better:
                    per_row = (lay, f, row)
            if per_row is not None:
                lay, pf, per_row = per_row
                _log(
                    f"[client] ==== PPT L1 check @ s={cs}B: per-layer({lay}, f={pf}) util "
                    f"{per_row['util_pct']}% (claim: <30%) vs chunk(f={cf}, {cf * cs}B) util "
                    f"{chunk_row['util_pct']}% (claim: >=60%) ===="
                )

        _log(f"[client] results in {csv_path} ({len(csv_rows)} rows this run)")

        handle.destroy()
        mf.get_and_clear_last_err_msg()
        assert mf.get_last_err_msg() == "", mf.get_last_err_msg()
        _log(f"[client rank {rank}] tensors unregistered, remote block released, exiting")
    finally:
        if ralloc_inited:
            ralloc.uninitialize(0)
        mf.uninitialize()
    _log("[client] all configs OK, client finished cleanly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
