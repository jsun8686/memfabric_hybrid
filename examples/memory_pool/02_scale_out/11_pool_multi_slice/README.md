# 11 — Pool Multi-Slice Expansion (device-scheduled RDMA)

Grow **one** pool to multiple slices per side: `extend_local_mem` ×2 + `extend_remote_mem` ×2
on the same handle. Each slice is registered as its own memory region, so every rank's
`ConnectRankInfo.memoryMap` holds two entries and the device-visible QP/MR table must expose
them through its per-rank MR slots (`MR_SLOTS_PER_RANK = 8`, range lookup picks the covering
slot). This example proves the kernel picks the right MR for **both** endpoints of a WQE.

## Topology

```
NEAR node (client, NPU dev)                  FAR node (daemon, NPU devs)
┌────────────────────────────┐               ┌────────────────────────────┐
│ pool id 0 (DEVICE_RDMA │   │   RoCE        │ pool id 0 contributor      │
│ DEVICE_SCHEDULE)           │ ────────────▶ │ slice 1: 64M DRAM slot     │
│ local slice 1: 64M (src1)  │   one-sided   │ slice 2: 64M DRAM slot     │
│ local slice 2: 64M (src2)  │   WRITE/READ  │ (each slice = own MR)      │
└────────────────────────────┘               └────────────────────────────┘
```

Same prerequisites as 09/10 (etcd-free store on the FAR node, `memfabric_hybrid` installed on
both nodes, control rpc port base aligned between daemon and client).

## Run

```bash
# FAR node: start the daemon (contributor per NPU card)
python3 memfabric_daemon.py --store tcp://<FAR_IP>:8588 --devs 0

# NEAR node: grow the pool and run the dual-slice copy flow
python3 memfabric_client.py --store tcp://<FAR_IP>:8588 --dev 0
```

## Flow

1. create the device-scheduled pool, then extend it **twice** on each side — two local
   slices (copy endpoints) and two FAR slices (landing zones); the client asserts the two
   remote slice gva ranges do **not** overlap and landed on the same FAR rank
2. host-copy two distinct int32 patterns into the local slices (`copy_data`)
3. warmup both `device_copy` writes on a side stream (loads the kernel library)
4. capture **both** writes into one NPU graph, replay N times
5. zero the local slices, READ both FAR slices back, host-fetch and compare — each slice
   must carry its **own** pattern (a slot mix-up would cross the patterns)
6. destroy the pool and exit with an empty error message

## Acceptance

| # | Line in `log/near_dev<N>.log` / behavior | Meaning |
|---|---|---|
| 1 | `pool grown to 2+2 slices` with two disjoint FAR gva ranges | same pool carries two block MRs per rank |
| 2 | `warmup writes OK (kernel library loaded, both slices reachable)` | both remote MR slots resolve before capture |
| 3 | `graph captured: 2 x ... device-scheduled WRITEs, one per slice` | both endpoints of each WQE address different MRs |
| 4 | `N replays done: ... GB/s device-scheduled` | replay path exercises the frozen MR table |
| 5 | `verify OK: both FAR slices carry their own pattern` | kernel range lookup picked the covering slot per endpoint |
| 6 | `pool multi-slice device-scheduled copies under NPU graph finished cleanly` | clean teardown, no error message |

FAR side: no WARN about truncated memory regions (`only the first 8 are visible`) — two
slices per rank stay well below the 8-slot table.

## Notes

- If a rank ever exceeds 8 slices, `FillQpInfo` warns (`only the first 8 are visible to
  device-scheduled kernels`) and truncates by address; host-path copies stay unaffected.
- The kernel-side MR table layout is a hard contract between this package's host library and
  its device headers (`dl_hccp_def.h`, `smem_ralloc_aicore_base_rdma.h`,
  `smem_shm_aicore_base_rdma.h`) — they ship together, do not mix versions.
