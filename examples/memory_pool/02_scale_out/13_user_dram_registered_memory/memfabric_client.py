#!/usr/bin/env python3
# coding=utf-8
# Copyright: (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
"""User-registered host-DRAM buffers as device-scheduled copy endpoints (10 + host memory).

Same topology and skeleton as 10_user_registered_memory, but the local copy endpoints are
4K-aligned anonymous mmap buffers on the host instead of NPU HBM tensors. The registered
host-DRAM MR carries a distinct device-dma base (HalHostRegister iova) in the v2 user-MR
table, so the AICore RDMA kernel addresses the SGE as regAddress + (localAddr - addr) under
the MR's lkey. Known issue under investigation: a user 4K-anon buffer as the device READ
source yields zeros regardless of write timing. Probe matrix A/B/B4/B5/B6/B6b/B7/C2 varies
the page state at register (unfaulted vs faulted+DRAM-resident), data residency at DMA
(dirty vs DRAM), page size and VA height (<2^47 vs python's default ~0xfffd... placement);
B7 OK would pin the constraint on the VA height (all known-good registrations live below
2^47), B6+B6b OK on the page-fault/invalidation handling.
Flow: register src/dst unfaulted -> fill+evict -> register b6 DRAM-resident ->
probes -> closed round-trip -> capture/replay the WRITE -> unregister ->
expect the host precheck to reject further copies.
"""

import argparse
import ctypes
import mmap
import os
import socket
import sys
import time

import memfabric_hybrid as mf
from memfabric_hybrid import ralloc

DEFAULT_WORLD = 512          # declared world capacity, actual members join dynamically
NIC_PORT_BASE = 10010        # data-plane nic port base (set_nic); NOT the control rpc port
RPC_PORT_BASE = 11105        # control rpc port = base + rankId (smem_ralloc_def.h default)
DEFAULT_SIZE = "1M"          # bytes per buffer / per one-sided copy (must be 4K aligned)
DEFAULT_REPLAYS = 8          # graph replays after capture
DEFAULT_BLOCK_SIZE = "64M"   # DRAM slot bytes committed on each side (FAR landing zone)
DEFAULT_MAX_POOL_SIZE = "4G" # pool DRAM window, must be GB aligned (VMM segment rule)
META_HBM_WINDOW = 1 << 30    # minimal GB-aligned hbm window: hosts the fixed device meta window only
EXTEND_RETRY_SEC = 5
EXTEND_TIMEOUT_SEC = 300
GIB = 1 << 30
DRAM_ALIGN = 4096
HUGEPAGE_2M = 2 * 1024 * 1024
EVICT_BYTES = 512 << 20    # cache-eviction sweep size (>> L3) for probe B4
LOW_VA_HINT = 0x300000000000  # 3TB: below bit 47, clear of the pool windows (0x288..-0x2a8..)
VA_BIT47 = 1 << 47

DATA_OP = ralloc.RallocDataOpType.DEVICE_RDMA | ralloc.RallocDataOpType.DEVICE_SCHEDULE
MEM_TYPE = ralloc.RallocMemType.HOST


_term_fd = None


def _log(msg):
    print(msg, flush=True)
    if _term_fd is not None:
        os.write(_term_fd, (msg + "\n").encode("utf-8", "replace"))


def _wait_tcp(url, timeout_sec):
    host, port = url.split("://", 1)[1].rsplit(":", 1)
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        with socket.socket() as s:
            if s.connect_ex((host, int(port))) == 0:
                return
        time.sleep(0.5)
    raise RuntimeError(f"endpoint not reachable within {timeout_sec}s: {url} — is the daemon up?")


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


def _map_buffer(size):
    """anonymous page-aligned mapping: the kernel guarantees 4K alignment for mmap bases.
    The ctypes view is transient so the mapping carries no long-lived buffer export --
    mm.close() then works on every exit path (no BufferError after exceptions)."""
    mm = mmap.mmap(-1, size)
    addr = ctypes.addressof(ctypes.c_char.from_buffer(mm))
    assert addr % DRAM_ALIGN == 0, f"mmap base 0x{addr:x} not page aligned"
    return mm, addr


def _bytes_at(addr, nbytes):
    """read n bytes of host memory at a raw address (pool HOST slots are MAP_FIXED host
    VMAs: their gva is directly CPU-readable, used by the probes as a trusted landing zone)"""
    return ctypes.string_at(addr, nbytes)


def _evict_caches(np, nbytes=EVICT_BYTES):
    """capacity-flood the CPU caches with an unrelated buffer so previously written lines
    (a probe source pattern) get evicted and written back to DRAM"""
    dummy = np.zeros(nbytes, dtype=np.uint8)
    dummy.fill(0xAB)
    del dummy


def _map_huge_buffer(size):
    """hugepage-backed mapping via libc mmap(MAP_HUGETLB); on failure fall back to a plain
    anonymous mapping advised MADV_HUGEPAGE (transparent huge pages, best-effort)"""
    libc = ctypes.CDLL("libc.so.6", use_errno=True)
    libc.mmap.restype = ctypes.c_void_p
    libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int,
                          ctypes.c_int, ctypes.c_long]
    prot_rw = 0x3
    map_private_anon = 0x22
    map_hugetlb = 0x40000
    addr = libc.mmap(None, size, prot_rw, map_private_anon | map_hugetlb, -1, 0)
    if addr is not None and addr != ctypes.c_void_p(-1).value:
        return addr, "MAP_HUGETLB"
    # fallback: plain anon + MADV_HUGEPAGE (THP may back it with 2M pages on fault)
    addr = libc.mmap(None, size, prot_rw, map_private_anon, -1, 0)
    if addr is None or addr == ctypes.c_void_p(-1).value:
        raise RuntimeError("mmap for the hugepage probe buffer failed")
    libc.madvise(ctypes.c_void_p(addr), ctypes.c_size_t(size), 14)  # MADV_HUGEPAGE
    return addr, "MADV_HUGEPAGE (THP fallback)"


def _unmap_buffer(addr, size):
    libc = ctypes.CDLL("libc.so.6", use_errno=True)
    libc.munmap(ctypes.c_void_p(addr), ctypes.c_size_t(size))


def _map_low_buffer(size):
    """plain anonymous mapping placed at a low VA hint (below bit 47): every registration
    that ever worked (HBM tensors, pool slots) lives below 2^47 while python's default mmap
    placement sits at ~0xfffd... above it -- this probe isolates the VA height. The hint is
    non-fixed: if the kernel maps elsewhere the verdict is skipped (see the run log)."""
    libc = ctypes.CDLL("libc.so.6", use_errno=True)
    libc.mmap.restype = ctypes.c_void_p
    libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int,
                          ctypes.c_int, ctypes.c_long]
    addr = libc.mmap(LOW_VA_HINT, size, 0x3, 0x22, -1, 0)  # PROT_READ|WRITE, MAP_PRIVATE|ANON
    if addr is None or addr == ctypes.c_void_p(-1).value:
        raise RuntimeError("mmap at the low-VA hint failed")
    return addr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--store", required=True,
                        help="store url of the FAR daemon, e.g. tcp://10.0.0.1:8588")
    parser.add_argument("--dev", type=int, required=True,
                        help="NPU id this client runs on")
    parser.add_argument("--size", default=DEFAULT_SIZE,
                        help=f"bytes per buffer / per one-sided copy, 4K aligned (K/M/G suffix, "
                             f"default {DEFAULT_SIZE})")
    parser.add_argument("--replays", type=int, default=DEFAULT_REPLAYS,
                        help=f"graph replays after capture (default {DEFAULT_REPLAYS})")
    parser.add_argument("--block-size", default=DEFAULT_BLOCK_SIZE,
                        help=f"DRAM slot bytes committed on each side as the FAR landing zone "
                             f"(K/M/G suffix, default {DEFAULT_BLOCK_SIZE})")
    parser.add_argument("--max-pool-size", default=DEFAULT_MAX_POOL_SIZE,
                        help=f"pool DRAM window declared to the FAR placement master, must be GB "
                             f"aligned (K/M/G suffix, default {DEFAULT_MAX_POOL_SIZE})")
    parser.add_argument("--world", type=int, default=DEFAULT_WORLD,
                        help=f"declared world capacity (default {DEFAULT_WORLD})")
    parser.add_argument("--rpc-port-base", type=int, default=RPC_PORT_BASE,
                        help=f"control rpc port base (default {RPC_PORT_BASE}); must match the daemon's value")
    parser.add_argument("--run-dir", default=None, help="client log dir (default ./log)")
    args = parser.parse_args()

    global _term_fd
    run_dir = os.path.abspath(args.run_dir or "./log")
    os.makedirs(run_dir, exist_ok=True)
    log = open(os.path.join(run_dir, f"near_dev{args.dev}.log"), "w")
    _term_fd = os.dup(1)
    os.dup2(log.fileno(), 1)
    os.dup2(log.fileno(), 2)
    _log(f"[client] run dir: {run_dir}, store: {args.store}, world: {args.world}, dev: {args.dev}")

    size = _parse_size(args.size)
    block = _parse_size(args.block_size)
    max_pool = _parse_size(args.max_pool_size)
    if size % DRAM_ALIGN != 0:
        raise RuntimeError(f"--size ({size}) must be {DRAM_ALIGN} aligned (host-DRAM MR rule)")
    if size % 4 != 0:
        raise RuntimeError(f"--size ({size}) must be 4-byte aligned (int32 verify pattern)")
    if size > block:
        raise RuntimeError(f"--size ({size}) exceeds --block-size ({block})")
    if 9 * size > block:
        raise RuntimeError(f"--block-size ({block}) must hold 9 probe windows of --size ({size})")
    if block > max_pool:
        raise RuntimeError(f"--block-size ({block}) exceeds --max-pool-size ({max_pool})")
    if max_pool % GIB != 0:
        raise RuntimeError(f"--max-pool-size ({max_pool}) must be GB aligned (VMM segment rule)")

    _wait_tcp(args.store, 30)
    dev = args.dev

    mf.set_log_level(1)
    assert mf.initialize() == 0, "mf.initialize failed"
    ralloc_inited = False
    src_mm = dst_mm = b6_mm = None
    hp_addr = 0
    low_addr = 0
    try:
        import numpy as np
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

        # device-scheduled pool on a DRAM window: the FAR side commits the landing slot.
        # max_hbm_size is GB aligned and hosts ONLY the fixed device meta window
        # (rank/QP/MR context); the user buffers below live in anonymous host mappings
        handle = ralloc.create(id=0, max_dram_size=max_pool, max_hbm_size=META_HBM_WINDOW, data_op_type=DATA_OP)
        _log(f"[client rank {rank}] device-scheduled pool created (DEVICE_RDMA | DEVICE_SCHEDULE)")

        ret, info = handle.extend_local_mem(MEM_TYPE, block)
        assert ret == 0 and info.get("gva"), f"extend_local_mem failed: {ret} {info}"
        local_gva = handle.get_mem_ptr_by_rank(rank, MEM_TYPE)
        assert local_gva != 0, "local DRAM slot not visible"

        deadline = time.time() + EXTEND_TIMEOUT_SEC
        while True:
            ret, info = handle.extend_remote_mem(MEM_TYPE, block)
            if ret == 0 and info.get("gva"):
                break
            if time.time() > deadline:
                raise RuntimeError(f"no FAR candidate within {EXTEND_TIMEOUT_SEC}s: {ret} {info}")
            time.sleep(EXTEND_RETRY_SEC)
        far_rank = info["rank_id"]
        far_gva = handle.get_mem_ptr_by_rank(far_rank, MEM_TYPE)
        assert far_gva != 0, "far DRAM slot not visible"
        mf.get_and_clear_last_err_msg()
        _log(f"[client rank {rank}] remote landing slot from FAR rank {far_rank} (gva=0x{far_gva:x}, npu {dev})")

        # ---- negative case 1: an unaligned host-DRAM address must be rejected at register time ----
        neg_mm, neg_base = _map_buffer(2 * DRAM_ALIGN)
        unaligned_addr = neg_base + DRAM_ALIGN + 128
        ret = handle.register(unaligned_addr, DRAM_ALIGN)
        assert ret != 0, "register() accepted an unaligned host-DRAM address: must be 4K aligned"
        mf.get_and_clear_last_err_msg()
        ret = handle.register(neg_base, DRAM_ALIGN // 2)  # aligned addr, unaligned size
        assert ret != 0, "register() accepted an unaligned host-DRAM size: must be a 4K multiple"
        mf.get_and_clear_last_err_msg()
        neg_mm.close()
        _log("[client] unaligned host-DRAM register rejected as expected (4K alignment rule)")

        # ---- host buffers as copy endpoints ----
        src_mm, src_addr = _map_buffer(size)
        dst_mm, dst_addr = _map_buffer(size)
        b6_mm, b6_addr = _map_buffer(size)
        assert src_addr != dst_addr and src_addr != b6_addr, "buffer mapping failed"
        words = size // 4
        seed = rank + 1
        expect = np.arange(words, dtype=np.int32) * 7 + seed
        expect6 = np.arange(words, dtype=np.int32) * 11 + 0x61
        expect6b = np.arange(words, dtype=np.int32) * 13 + 0x62
        _log(f"[client] host buffers: src=0x{src_addr:x} dst=0x{dst_addr:x} b6=0x{b6_addr:x} "
             f"({size} bytes each, page-aligned anonymous mmap)")

        # B premise: src/dst registered UNFAULTED (mapped but never touched)
        assert handle.register(src_addr, size) == 0, "register(src buffer) failed"
        assert handle.register(dst_addr, size) == 0, "register(dst buffer) failed"
        _log("[client] src/dst registered unfaulted (user MR table v2: regAddress = HalHostRegister iova)")

        # fill everything, evict to DRAM, THEN register b6 (B6 premise: faulted +
        # DRAM-resident data at registration); finally re-dirty src (B premise: its
        # pattern is cache-dirty at DMA time) and leave dst clean-zeroed for the C2 sink
        np.frombuffer(src_mm, dtype=np.int32)[:] = expect
        np.frombuffer(dst_mm, dtype=np.int32)[:] = 0
        np.frombuffer(b6_mm, dtype=np.int32)[:] = expect6
        _evict_caches(np)  # sweep 0: all three patterns DRAM-resident, caches clean
        assert handle.register(b6_addr, size) == 0, "register(b6 buffer) failed"
        np.frombuffer(src_mm, dtype=np.int32)[:] = expect  # re-dirty src for probe B
        _log("[client] b6 registered DRAM-resident; src re-dirtied (per-probe premises set)")

        # ---- probe matrix (the first device_copy also dlopens and loads the kernel
        # library, which is illegal inside capture). Three device phases on one side
        # stream with CPU sweeps in between; per-probe factors under test = page state
        # at register (unfaulted vs faulted+DRAM-resident), data residency at DMA
        # (cache-dirty vs DRAM), page size:
        # A:   pool slot source, 1M cache-dirty write -> DMA (baseline: pool iova reads
        #      are coherent with CPU-dirty lines);
        # B:   user 4K-anon, registered unfaulted, cache-dirty post-register fill;
        # B4:  same buffer after a 512MB eviction sweep (pattern forced to DRAM): FAIL =>
        #      registration diverged from the pages (zero-page pin + COW), OK => non-coherent;
        # B5:  hugepage-backed user buffer (MAP_HUGETLB / THP fallback), post-register fill;
        # B6:  user 4K-anon, faulted + pattern + evicted BEFORE register (DRAM-resident,
        #      clean cache at registration): OK => translation aliases prepared pages;
        # B6b: SAME buffer, cache-dirty post-register REWRITE (distinct pattern): OK =>
        #      reads are coherent once pages are connected => the hazards are registration-
        #      time invalidation + unfaulted registration => library fix = touch pages
        #      before HalHostRegister (LvaShmReservePhysicalMemory equivalent);
        # C2:  user sink, far seeded via a pool->pool write, dst pre-evicted clean.
        side = torch.npu.Stream()
        side.wait_stream(torch.npu.current_stream())
        probeA = np.arange(words, dtype=np.int32) * 3 + 0x41410
        ctypes.memmove(local_gva, probeA.tobytes(), size)              # A source
        ctypes.memmove(local_gva + 4 * size, expect.tobytes(), size)   # C2 seed source
        hp_addr, hp_kind = _map_huge_buffer(HUGEPAGE_2M)
        assert handle.register(hp_addr, HUGEPAGE_2M) == 0, "register(hugepage buffer) failed"
        ctypes.memmove(hp_addr, expect.tobytes(), size)                # B5 source, post-register fill
        _log(f"[client] B5 buffer: 0x{hp_addr:x} ({HUGEPAGE_2M} bytes, {hp_kind}, "
             f"2M-aligned={hp_addr % HUGEPAGE_2M == 0})")
        low_addr = _map_low_buffer(size)
        low_hint_ok = low_addr < VA_BIT47
        assert handle.register(low_addr, size) == 0, "register(low-VA buffer) failed"
        ctypes.memmove(low_addr, expect.tobytes(), size)               # B7 source, post-register fill
        _log(f"[client] B7 buffer: 0x{low_addr:x} ({size} bytes, low-VA hint honored={low_hint_ok})")
        with torch.npu.stream(side):
            stream_ptr = torch.npu.current_stream().npu_stream
            assert handle.device_copy(local_gva, far_gva, size, stream_ptr) == 0, "probe A write failed"
            assert handle.device_copy(far_gva, local_gva + size, size, stream_ptr) == 0, "probe A readback failed"
            assert handle.device_copy(src_addr, far_gva, size, stream_ptr) == 0, "probe B write failed"
            assert handle.device_copy(far_gva, local_gva + 2 * size, size, stream_ptr) == 0, \
                "probe B readback failed"
        torch.npu.synchronize()
        _evict_caches(np)  # sweep 1: force the src pattern out to DRAM
        with torch.npu.stream(side):
            stream_ptr = torch.npu.current_stream().npu_stream
            assert handle.device_copy(src_addr, far_gva, size, stream_ptr) == 0, "probe B4 write failed"
            assert handle.device_copy(far_gva, local_gva + 3 * size, size, stream_ptr) == 0, \
                "probe B4 readback failed"
            assert handle.device_copy(hp_addr, far_gva, size, stream_ptr) == 0, "probe B5 write failed"
            assert handle.device_copy(far_gva, local_gva + 5 * size, size, stream_ptr) == 0, \
                "probe B5 readback failed"
            assert handle.device_copy(low_addr, far_gva, size, stream_ptr) == 0, "probe B7 write failed"
            assert handle.device_copy(far_gva, local_gva + 8 * size, size, stream_ptr) == 0, \
                "probe B7 readback failed"
            assert handle.device_copy(b6_addr, far_gva, size, stream_ptr) == 0, "probe B6 write failed"
            assert handle.device_copy(far_gva, local_gva + 6 * size, size, stream_ptr) == 0, \
                "probe B6 readback failed"
        torch.npu.synchronize()
        ctypes.memmove(b6_addr, expect6b.tobytes(), size)  # dirty post-register rewrite
        with torch.npu.stream(side):
            stream_ptr = torch.npu.current_stream().npu_stream
            assert handle.device_copy(b6_addr, far_gva, size, stream_ptr) == 0, "probe B6b write failed"
            assert handle.device_copy(far_gva, local_gva + 7 * size, size, stream_ptr) == 0, \
                "probe B6b readback failed"
            assert handle.device_copy(local_gva + 4 * size, far_gva, size, stream_ptr) == 0, \
                "probe C2 seed write failed"
            assert handle.device_copy(far_gva, dst_addr, size, stream_ptr) == 0, "probe C2 read failed"
        torch.npu.synchronize()

        aGot = np.frombuffer(_bytes_at(local_gva + size, size), dtype=np.int32)
        bFar = np.frombuffer(_bytes_at(local_gva + 2 * size, size), dtype=np.int32)
        b4Far = np.frombuffer(_bytes_at(local_gva + 3 * size, size), dtype=np.int32)
        b5Far = np.frombuffer(_bytes_at(local_gva + 5 * size, size), dtype=np.int32)
        b6Far = np.frombuffer(_bytes_at(local_gva + 6 * size, size), dtype=np.int32)
        b6bFar = np.frombuffer(_bytes_at(local_gva + 7 * size, size), dtype=np.int32)
        b7Far = np.frombuffer(_bytes_at(local_gva + 8 * size, size), dtype=np.int32)
        c2Dst = np.frombuffer(bytes(dst_mm[:]), dtype=np.int32)
        okA = np.array_equal(aGot, probeA)
        okB = np.array_equal(bFar, expect)
        okB4 = np.array_equal(b4Far, expect)
        okB5 = np.array_equal(b5Far, expect)
        okB6 = np.array_equal(b6Far, expect6)
        okB6b = np.array_equal(b6bFar, expect6b)
        okC2 = np.array_equal(c2Dst, expect)
        if not low_hint_ok:
            okB7 = True
            _log(f"[probe B7] SKIP: low-VA hint not honored, mapped at 0x{low_addr:x}")
        else:
            okB7 = np.array_equal(b7Far, expect)
            _log("[probe B7] user buffer at low VA (<2^47) as WRITE source: " + ("OK" if okB7 else "FAIL")
                 + ("" if okB7 else f", far={b7Far[:8].tolist()} want={expect[:8].tolist()}"))
        _log("[probe A] pool slot source, cache-dirty (baseline): " + ("OK" if okA else "FAIL")
             + ("" if okA else f", got={aGot[:8].tolist()} want={probeA[:8].tolist()}"))
        _log("[probe B] user 4K-anon, registered unfaulted, dirty fill: " + ("OK" if okB else "FAIL")
             + ("" if okB else f", far={bFar[:8].tolist()} want={expect[:8].tolist()}"))
        _log("[probe B4] same source after 512MB eviction (DRAM-resident): "
             + ("OK" if okB4 else "FAIL")
             + ("" if okB4 else f", far={b4Far[:8].tolist()} want={expect[:8].tolist()}"))
        _log("[probe B5] hugepage-backed user buffer as source: " + ("OK" if okB5 else "FAIL")
             + ("" if okB5 else f", far={b5Far[:8].tolist()} want={expect[:8].tolist()}"))
        _log("[probe B6] user 4K-anon, DRAM-resident at register: " + ("OK" if okB6 else "FAIL")
             + ("" if okB6 else f", far={b6Far[:8].tolist()} want={expect6[:8].tolist()}"))
        _log("[probe B6b] same buffer, dirty post-register rewrite: " + ("OK" if okB6b else "FAIL")
             + ("" if okB6b else f", far={b6bFar[:8].tolist()} want={expect6b[:8].tolist()}"))
        _log("[probe C2] user sink, far seeded via pool (dst pre-evicted): "
             + ("OK" if okC2 else "FAIL")
             + ("" if okC2 else f", dst={c2Dst[:8].tolist()} want={expect[:8].tolist()}"))
        assert okA and okB and okB4 and okB5 and okB6 and okB6b and okB7 and okC2, \
            f"probes failed: A={okA} B={okB} B4={okB4} B5={okB5} B6={okB6} B6b={okB6b} B7={okB7} " \
            f"C2={okC2} (B7 OK => VA-height constraint, allocate user buffers below 2^47; " \
            f"B6/B6b OK => library fix = touch pages before HalHostRegister; see README)"
        _log("[client] probes A/B/B4/B5/B6/B6b/B7/C2 OK (kernel library loaded)")

        # closed round-trip user -> FAR -> user2 (the original warmup, now with a value dump)
        np.frombuffer(dst_mm, dtype=np.int32)[:] = 0
        with torch.npu.stream(side):
            stream_ptr = torch.npu.current_stream().npu_stream
            assert handle.device_copy(src_addr, far_gva, size, stream_ptr) == 0, "warmup write failed"
            assert handle.device_copy(far_gva, dst_addr, size, stream_ptr) == 0, "warmup read failed"
        torch.npu.synchronize()
        got = np.frombuffer(bytes(dst_mm[:]), dtype=np.int32)
        assert np.array_equal(got, expect), \
            f"warmup round-trip mismatch: got={got[:8].tolist()} want={expect[:8].tolist()}"
        _log("[client] warmup round-trip OK (host buffer -> FAR slot -> host buffer2)")

        # capture the WRITE into an NPU graph: host buffer -> FAR pool slot + quiet
        graph = torch.npu.NPUGraph()
        torch.npu.synchronize()
        with torch.npu.stream(side):
            graph.capture_begin()
            assert handle.device_copy(src_addr, far_gva, size,
                                      torch.npu.current_stream().npu_stream) == 0, \
                "captured copy submit failed"
            graph.capture_end()
        _log(f"[client] graph captured: 1 x {size} byte device-scheduled WRITE from the host buffer, "
             f"no host interaction inside")

        t0 = time.perf_counter()
        for _ in range(args.replays):
            graph.replay()
        torch.npu.synchronize()
        t_replay = time.perf_counter() - t0
        moved = args.replays * size
        _log(f"[client] {args.replays} replays done: {moved / GIB:.2f} GiB in {t_replay * 1e3:.2f} ms, "
             f"{moved / t_replay / GIB:.2f} GB/s device-scheduled")

        # verify on the default stream: READ the far slot back into the second buffer
        np.frombuffer(dst_mm, dtype=np.int32)[:] = 0
        assert handle.device_copy(far_gva, dst_addr, size, 0) == 0, "verify read failed"
        torch.npu.synchronize()
        got = np.frombuffer(bytes(dst_mm[:]), dtype=np.int32)
        assert np.array_equal(got, expect), \
            f"verify failed: buffer2 != pattern after {args.replays} replays"
        _log(f"[client] verify OK: FAR rank {far_rank} slot matches the host pattern "
             f"({words} words, seed {seed})")

        # ---- negative case 2: after unregister the copy must fail the host precheck ----
        assert handle.unregister(dst_addr) == 0, "unregister(dst buffer) failed"
        ret = handle.device_copy(far_gva, dst_addr, size, 0)
        assert ret != 0, "device_copy into an unregistered buffer must fail"
        mf.get_and_clear_last_err_msg()
        _log("[client] post-unregister copy rejected as expected (host precheck)")

        assert handle.unregister(src_addr) == 0, "unregister(src buffer) failed"
        assert handle.unregister(b6_addr) == 0, "unregister(b6 buffer) failed"
        assert handle.unregister(low_addr) == 0, "unregister(low-VA buffer) failed"
        assert handle.unregister(hp_addr) == 0, "unregister(hugepage buffer) failed"
        _unmap_buffer(hp_addr, HUGEPAGE_2M)
        hp_addr = 0
        _unmap_buffer(low_addr, size)
        low_addr = 0
        b6_mm.close()
        b6_mm = None
        dst_mm.close()
        src_mm.close()
        dst_mm = src_mm = None
        handle.destroy()
        mf.get_and_clear_last_err_msg()
        assert mf.get_last_err_msg() == "", mf.get_last_err_msg()
        _log(f"[client rank {rank}] buffers unregistered, remote block released, exiting")
    finally:
        if hp_addr != 0:
            _unmap_buffer(hp_addr, HUGEPAGE_2M)
        if low_addr != 0:
            _unmap_buffer(low_addr, size)
        if b6_mm is not None:
            b6_mm.close()
        if dst_mm is not None:
            dst_mm.close()
        if src_mm is not None:
            src_mm.close()
        if ralloc_inited:
            ralloc.uninitialize(0)
        mf.uninitialize()
    _log("[client] user-registered host-DRAM endpoints under NPU graph finished cleanly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
