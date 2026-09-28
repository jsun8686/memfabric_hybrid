#!/usr/bin/env python3
# coding=utf-8
# Copyright: (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
"""User-registered host-DRAM buffers as device-scheduled copy endpoints (10 + host memory).

Same topology and skeleton as 10_user_registered_memory, but the local copy endpoints are
4K-aligned anonymous mmap buffers on the host instead of NPU HBM tensors. The registered
host-DRAM MR carries a distinct device-dma base (HalHostRegister iova) in the v2 user-MR
table, so the AICore RDMA kernel addresses the SGE as regAddress + (localAddr - addr) under
the MR's lkey. Known issue under investigation: a 4K-anon user buffer as the device READ
source yields zeros regardless of write timing; probes B4 (512MB cache-eviction sweep) and
B5 (hugepage-backed buffer) discriminate non-coherent user-iova reads vs wrong-page pinning.
Flow: reject an unaligned register -> register buffers -> fill post-register -> probes
A/B/B4/B5/C2 -> closed round-trip -> capture/replay the WRITE -> unregister ->
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
    if 6 * size > block:
        raise RuntimeError(f"--block-size ({block}) must hold 6 probe windows of --size ({size})")
    if block > max_pool:
        raise RuntimeError(f"--block-size ({block}) exceeds --max-pool-size ({max_pool})")
    if max_pool % GIB != 0:
        raise RuntimeError(f"--max-pool-size ({max_pool}) must be GB aligned (VMM segment rule)")

    _wait_tcp(args.store, 30)
    dev = args.dev

    mf.set_log_level(1)
    assert mf.initialize() == 0, "mf.initialize failed"
    ralloc_inited = False
    src_mm = dst_mm = None
    hp_addr = 0
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
        assert src_addr != dst_addr, "buffer mapping failed"
        words = size // 4
        seed = rank + 1
        expect = np.arange(words, dtype=np.int32) * 7 + seed
        _log(f"[client] host buffers: src=0x{src_addr:x} dst=0x{dst_addr:x} ({size} bytes each, "
             f"page-aligned anonymous mmap)")

        assert handle.register(src_addr, size) == 0, "register(src buffer) failed"
        assert handle.register(dst_addr, size) == 0, "register(dst buffer) failed"
        _log("[client] both host buffers registered (user MR table v2: regAddress = HalHostRegister iova)")

        # fill the buffers AFTER registration as the fixed probe protocol -- note the write
        # ordering around register() has been proven irrelevant (the zero-read from a 4K-anon
        # user source happens either way); root cause is discriminated by probes B4/B5
        np.frombuffer(src_mm, dtype=np.int32)[:] = expect
        np.frombuffer(dst_mm, dtype=np.int32)[:] = 0
        _log("[client] pattern filled after register (fixed protocol; write timing proven irrelevant)")

        # ---- probes, OUTSIDE any graph (the first device_copy also dlopens and loads the
        # kernel library, which is illegal inside capture). Two device phases with a CPU
        # cache-eviction sweep in between; the single side stream keeps everything ordered:
        # A:  pool->pool round-trip incl. CPU visibility of NIC-written pool pages (baseline,
        #     known-good legs in both directions);
        # B:  user 4K-anon buffer as WRITE source, pattern filled post-register;
        # B4: SAME source re-sent after a 512MB eviction sweep (pattern forced to DRAM):
        #     OK => user-iova reads are non-coherent (DRAM-only), FAIL => wrong pages pinned;
        # B5: hugepage-backed user buffer (MAP_HUGETLB, THP fallback) as WRITE source:
        #     OK => page-size/VMA sensitivity, practical workaround = hugepage buffers;
        # C2: user buffer as READ sink, far seeded via a pool->pool write (self-sufficient).
        # Every probe runs unconditionally so each leg logs its own verdict. ----
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
        with torch.npu.stream(side):
            stream_ptr = torch.npu.current_stream().npu_stream
            assert handle.device_copy(local_gva, far_gva, size, stream_ptr) == 0, "probe A write failed"
            assert handle.device_copy(far_gva, local_gva + size, size, stream_ptr) == 0, "probe A readback failed"
            assert handle.device_copy(src_addr, far_gva, size, stream_ptr) == 0, "probe B write failed"
            assert handle.device_copy(far_gva, local_gva + 2 * size, size, stream_ptr) == 0, \
                "probe B readback failed"
        torch.npu.synchronize()
        _evict_caches(np)  # force the source pattern out of L1/L2/L3 into DRAM
        with torch.npu.stream(side):
            stream_ptr = torch.npu.current_stream().npu_stream
            assert handle.device_copy(src_addr, far_gva, size, stream_ptr) == 0, "probe B4 write failed"
            assert handle.device_copy(far_gva, local_gva + 3 * size, size, stream_ptr) == 0, \
                "probe B4 readback failed"
            assert handle.device_copy(hp_addr, far_gva, size, stream_ptr) == 0, "probe B5 write failed"
            assert handle.device_copy(far_gva, local_gva + 5 * size, size, stream_ptr) == 0, \
                "probe B5 readback failed"
            assert handle.device_copy(local_gva + 4 * size, far_gva, size, stream_ptr) == 0, \
                "probe C2 seed write failed"
            assert handle.device_copy(far_gva, dst_addr, size, stream_ptr) == 0, "probe C2 read failed"
        torch.npu.synchronize()

        aGot = np.frombuffer(_bytes_at(local_gva + size, size), dtype=np.int32)
        bFar = np.frombuffer(_bytes_at(local_gva + 2 * size, size), dtype=np.int32)
        b4Far = np.frombuffer(_bytes_at(local_gva + 3 * size, size), dtype=np.int32)
        b5Far = np.frombuffer(_bytes_at(local_gva + 5 * size, size), dtype=np.int32)
        c2Dst = np.frombuffer(bytes(dst_mm[:]), dtype=np.int32)
        okA = np.array_equal(aGot, probeA)
        okB = np.array_equal(bFar, expect)
        okB4 = np.array_equal(b4Far, expect)
        okB5 = np.array_equal(b5Far, expect)
        okC2 = np.array_equal(c2Dst, expect)
        _log("[probe A] pool->pool round-trip (baseline + CPU visibility): " + ("OK" if okA else "FAIL")
             + ("" if okA else f", got={aGot[:8].tolist()} want={probeA[:8].tolist()}"))
        _log("[probe B] user 4K-anon as WRITE source: " + ("OK" if okB else "FAIL")
             + ("" if okB else f", far={bFar[:8].tolist()} want={expect[:8].tolist()}"))
        _log("[probe B4] same source after 512MB cache eviction (DRAM-resident): "
             + ("OK" if okB4 else "FAIL")
             + ("" if okB4 else f", far={b4Far[:8].tolist()} want={expect[:8].tolist()}"))
        _log("[probe B5] hugepage-backed user buffer as WRITE source: " + ("OK" if okB5 else "FAIL")
             + ("" if okB5 else f", far={b5Far[:8].tolist()} want={expect[:8].tolist()}"))
        _log("[probe C2] user buffer as READ sink (far seeded via pool): " + ("OK" if okC2 else "FAIL")
             + ("" if okC2 else f", dst={c2Dst[:8].tolist()} want={expect[:8].tolist()}"))
        assert okA and okB and okB4 and okB5 and okC2, \
            f"device-scheduled copy probes failed: A={okA} B={okB} B4={okB4} B5={okB5} C2={okC2} " \
            f"(B4 OK => non-coherent reads; B4 FAIL + B5 OK => page-size sensitive; see README)"
        _log("[client] probes A/B/B4/B5/C2 OK (kernel library loaded)")

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
        assert handle.unregister(hp_addr) == 0, "unregister(hugepage buffer) failed"
        _unmap_buffer(hp_addr, HUGEPAGE_2M)
        hp_addr = 0
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
