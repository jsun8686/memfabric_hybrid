#!/usr/bin/env python3
# coding=utf-8
# Copyright: (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
"""NVMe SSD pool contributor (FAR role) for example 16.

The FAR node hosts the SPDK nvmf target (see setup_nvmf_tgt.sh) and runs this daemon:
it only joins the ralloc control plane as a FAR contributor -- when a NEAR client
requests an NVMe block (extend_remote_mem(NVME)), the placement master picks this
node, the local HybmNvmeSegment carves an LBA extent, signs a namespace over the
SPDK JSON-RPC socket and exports the nvmf identity to the pool group. No data-plane
library is needed on this side (the SPDK initiator lives in the NEAR process).

Required env (printed by setup_nvmf_tgt.sh, usually via `daemon env : ...`):
    MF_NVME_NQN / MF_NVME_TRADDR / MF_NVME_TRSVCID / MF_NVME_RPC_SOCK /
    MF_NVME_BASE_BDEV / MF_NVME_LBA_BASE / MF_NVME_EXTENT_GRAN

Simulation hint: with the default 1G extent granularity a malloc bdev of 1G can host
exactly one extent; export MF_NVME_EXTENT_GRAN=64M (or raise SIM_SIZE_MB) so several
smaller blocks fit.
"""

import argparse
import os
import signal
import socket
import sys
import time

import memfabric_hybrid as mf
from memfabric_hybrid import ralloc

DEFAULT_WORLD = 512          # declared world capacity, actual members join dynamically
NIC_PORT_BASE = 10005        # data-plane nic port base (set_nic); NOT the control rpc port
RPC_PORT_BASE = 11100        # control rpc port = base + rankId (smem_ralloc_def.h default)

NVME_ENV_KEYS = ("MF_NVME_NQN", "MF_NVME_TRADDR", "MF_NVME_TRSVCID", "MF_NVME_RPC_SOCK",
                 "MF_NVME_BASE_BDEV", "MF_NVME_LBA_BASE", "MF_NVME_EXTENT_GRAN",
                 "MF_NVME_TRSTRING")


def _log(msg):
    print(msg, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--store", required=True,
                        help="store url, MUST point at THIS FAR node, e.g. tcp://10.0.0.1:8588")
    parser.add_argument("--world", type=int, default=DEFAULT_WORLD,
                        help=f"declared world capacity (default {DEFAULT_WORLD})")
    parser.add_argument("--rpc-port-base", type=int, default=RPC_PORT_BASE,
                        help=f"control rpc port base (port = base + rank_id, default {RPC_PORT_BASE}); "
                             f"keep one value for the whole session, clients included")
    args = parser.parse_args()

    _log("[daemon] NVMe pool contributor (FAR role)")
    _log(f"[daemon] store: {args.store}, world: {args.world}")
    for key in NVME_ENV_KEYS:
        _log(f"[daemon] env {key}={os.environ.get(key, '<unset>')}")

    mf.set_log_level(1)
    assert mf.initialize() == 0, "mf.initialize failed"
    ralloc_inited = False
    try:
        cfg = ralloc.RallocConfig()
        cfg.auto_ranking = True
        cfg.role = ralloc.RallocRole.FAR
        cfg.start_store = True
        cfg.dynamic_world_size = True
        cfg.rpc_port_base = args.rpc_port_base
        cfg.set_nic(f"tcp://{socket.gethostbyname(socket.gethostname())}:{NIC_PORT_BASE}")
        assert ralloc.initialize(args.store, args.world, 0, cfg) == 0, "ralloc.initialize failed"
        ralloc_inited = True
        rank = ralloc.get_rank_id()
        _log(f"[daemon] contributor ready: rank {rank} — waiting for NVMe block requests")

        stop = {"flag": False}

        def _stop_signal(_signum, _frame):
            stop["flag"] = True

        signal.signal(signal.SIGTERM, _stop_signal)
        signal.signal(signal.SIGINT, _stop_signal)
        while not stop["flag"]:
            time.sleep(1.0)
    finally:
        if ralloc_inited:
            ralloc.uninitialize(0)
        mf.uninitialize()
    _log("[daemon] contributor stopped cleanly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
