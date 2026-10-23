#!/usr/bin/env bash
# NVMe-oF target bootstrap for the memfabric NVMe pool (FAR node, one-time).
# Loads SPDK's nvmf_tgt as a systemd service (or starts it directly when the
# environment has no systemd, e.g. containers) and exposes the local NVMe bdevs
# as an RDMA subsystem. The memfabric daemon only signs LBA extents (namespaces)
# over the local JSON-RPC socket; identity env for the daemon:
#   export MF_NVME_NQN=<subsystem nqn printed below>
#   export MF_NVME_TRADDR=<this node's storage-plane IP>
#   export MF_NVME_TRSVCID=4420            # default
#   export MF_NVME_RPC_SOCK=/var/tmp/spdk.sock
#   export MF_NVME_BASE_BDEV=Nvme0n1      # bdev backing the pool (raided bdev for multi-drive)
#   export MF_NVME_LBA_BASE=0             # disjoint nonzero bases when several FAR ranks share one node
#
# Disk-less simulation (same host, loopback, no NVMe drive / no RoCE NIC needed):
#   sudo SPDK_ROOT=/path/to/spdk bash ./setup_nvmf_tgt.sh 127.0.0.1 --sim
#   - backs the pool with an in-memory malloc bdev (SIM_SIZE_MB, default 1024)
#   - speaks NVMe/TCP instead of RDMA on the listener
#   - initiator side then declares the protocol with the pool op bit:
#     ralloc.create(..., data_op_type=... | RallocDataOpType.NVME_TCP)
#
# SPDK_ROOT (optional): points at an SPDK source tree -- the script then uses
#   ${SPDK_ROOT}/build/bin/nvmf_tgt and ${SPDK_ROOT}/scripts/rpc.py directly and
#   prints the LD_LIBRARY_PATH the NEAR side needs for dlopen. Without SPDK_ROOT
#   a package install layout is assumed (systemd unit + rpc.py on PATH).
#
# Usage: sudo [SPDK_ROOT=...] bash ./setup_nvmf_tgt.sh <traddr> [bdev] [--sim] [--tcp|--rdma|--transport=TCP|RDMA]
#   transport defaults to TCP with --sim (loopback) and RDMA otherwise;
#   "--sim --rdma" = simulated malloc disk behind a real RDMA listener (cross-node)
# (bash is required; invoking via sh re-execs bash automatically)

# re-exec under bash when mistakenly invoked via sh/dash (dash lacks pipefail)
[ -z "${BASH_VERSION:-}" ] && exec bash "$0" "$@"
set -eu

TRADDR=""
BASE_BDEV=""
SIM=0
TRANSPORT=""
for arg in "$@"; do
    case "${arg}" in
        --sim) SIM=1 ;;
        --tcp|--transport=TCP|--transport=tcp) TRANSPORT="TCP" ;;
        --rdma|--transport=RDMA|--transport=rdma) TRANSPORT="RDMA" ;;
        *) if [ -z "${TRADDR}" ]; then TRADDR="${arg}"; else BASE_BDEV="${arg}"; fi ;;
    esac
done
: "${TRADDR:?usage: $0 <traddr> [bdev] [--sim] [--tcp|--rdma|--transport=TCP|RDMA]}"

if [ -z "${BASE_BDEV}" ]; then
    # sim default is the malloc bdev created below, real default is the first NVMe drive
    if [ "${SIM}" = 1 ]; then BASE_BDEV="Malloc0"; else BASE_BDEV="Nvme0n1"; fi
fi
TRSVCID=4420
NQN="nqn.2016-06.io.spdk:memfabric"
SIM_SIZE_MB="${SIM_SIZE_MB:-1024}"
# transport defaults to loopback TCP for the disk-less simulation, RDMA otherwise;
# "--sim --rdma" combines a simulated malloc bdev with a real RDMA listener
# (cross-node runs: traddr = this node's RoCE IP, build SPDK with --with-rdma)
if [ -z "${TRANSPORT}" ]; then
    if [ "${SIM}" = 1 ]; then TRANSPORT="TCP"; else TRANSPORT="RDMA"; fi
fi
RPC_SOCK=/var/tmp/spdk.sock

SPDK_ROOT="${SPDK_ROOT:-}"
if [ -n "${SPDK_ROOT}" ]; then
    NVMF_TGT="${SPDK_ROOT}/build/bin/nvmf_tgt"
    RPC_PY="${SPDK_ROOT}/scripts/rpc.py"
    [ -x "${NVMF_TGT}" ] || { echo "SPDK_ROOT set but ${NVMF_TGT} not found (built?)" >&2; exit 1; }
    [ -f "${RPC_PY}" ] || { echo "SPDK_ROOT set but ${RPC_PY} not found" >&2; exit 1; }
else
    NVMF_TGT="/usr/libexec/spdk/nvmf_tgt"
    RPC_PY="$(command -v spdk_rpc 2>/dev/null || echo rpc.py)"
fi
RPC="python3 ${RPC_PY} -s ${RPC_SOCK}"

# 0) the malloc bdev (sim) and the RDMA transport buffers both need DPDK hugepages
#    (4G of 2M pages, no-op if already there)
if [ "${SIM}" = 1 ] || [ "${TRANSPORT}" = "RDMA" ]; then
    HUGEPD=/sys/kernel/mm/hugepages/hugepages-2048kB
    WANT=2048
    HAVE=$(cat "${HUGEPD}/nr_hugepages")
    if [ "${HAVE}" -lt "${WANT}" ]; then
        echo "${WANT}" > "${HUGEPD}/nr_hugepages" || {
            echo "cannot raise hugepages to ${WANT} (have ${HAVE}); run with sudo, free memory, or set SIM_SIZE_MB smaller" >&2
            exit 1
        }
    fi
fi

# 0.5) NUMA-local reactors: the tgt's own DMA (NIC <-> bdev memory) should stay on
#      the data NIC's socket -- cross-node traffic costs real bandwidth/latency on
#      multi-socket ARM. RDMA resolves the NIC from the listener IP and takes the
#      node's first 2 cpus (stock 2-reactor footprint, just on the right socket);
#      TCP loopback sim keeps the historical cpus 0-1.
#      NOTE: -c/-m coremasks carry LCORE ids, which must stay < RTE_MAX_LCORE (128 in
#      this build) -- a NIC on node 6 has physical cpus 144+, so the pinning rides
#      --lcores "0@<phys>,1@<phys>" (lcore id mapped to the physical cpu) instead.
EAL_CORES="-m 0x3"
if [ "${TRANSPORT}" = "RDMA" ]; then
    NIC_IF=$(ip -o -4 addr show | awk -v ip="${TRADDR}" '{split($4,a,"/"); if (a[1]==ip) {print $2; exit}}')
    if [ -n "${NIC_IF}" ] && [ -r "/sys/class/net/${NIC_IF}/device/numa_node" ]; then
        NIC_NUMA=$(cat "/sys/class/net/${NIC_IF}/device/numa_node" 2>/dev/null || echo -1)
        if [ "${NIC_NUMA}" -ge 0 ] 2>/dev/null; then
            PINNED=$(python3 - "${NIC_NUMA}" <<'PY'
import sys
node = sys.argv[1]
raw = open("/sys/devices/system/node/node%s/cpulist" % node).read().strip()
cpus = []
for p in raw.split(","):
    if "-" in p:
        a, b = (int(x) for x in p.split("-"))
        cpus.extend(range(a, b + 1))
    else:
        cpus.append(int(p))
if len(cpus) >= 2:
    print("--lcores 0@%d,1@%d" % (cpus[0], cpus[1]))
PY
)
            if [ -n "${PINNED}" ]; then
                EAL_CORES="${PINNED}"
                echo "nvmf_tgt reactors pinned to NIC ${NIC_IF} (NUMA node ${NIC_NUMA}, ${EAL_CORES})"
            fi
        fi
    fi
    if [ "${EAL_CORES}" = "-m 0x3" ]; then
        echo "NOTE: NIC NUMA node unresolved (no iface for ${TRADDR}?), reactors stay on cpus 0-1"
    fi
fi

# 1) target daemon: systemd service when available, direct nohup launch otherwise
#    (containers usually have no systemd as PID 1)
start_tgt_direct() {
    # a --with-shared build resolves its own module libs from <spdk>/build/lib and the
    # DPDK libs from <spdk>/dpdk/build/lib -- prepend BOTH so a bare invocation without
    # an exported LD_LIBRARY_PATH still works
    local ld_path="${LD_LIBRARY_PATH:-}"
    if [ -n "${SPDK_ROOT}" ]; then
        local spdk_lib="${SPDK_ROOT}/build/lib"
        local dpdk_lib="${SPDK_ROOT}/dpdk/build/lib"
        [ -d "${spdk_lib}" ] && ld_path="${spdk_lib}:${ld_path}"
        [ -d "${dpdk_lib}" ] && ld_path="${dpdk_lib}:${ld_path}"
    fi
    # ${EAL_CORES} stays intentionally unquoted: it is a multi-token argument string
    # ("-m 0x3" fallback or "--lcores 0@144,1@145") and must word-split
    LD_LIBRARY_PATH="${ld_path}" nohup "${NVMF_TGT}" ${EAL_CORES} -s 2048 -r "${RPC_SOCK}" >/tmp/nvmf_tgt.log 2>&1 &
    echo "nvmf_tgt started directly (no systemd in this environment)"
    echo "  log: /tmp/nvmf_tgt.log   stop: pkill nvmf_tgt"
}

# systemd path only when systemd actually runs (PID 1): a container with the
# systemctl binary but no systemd manager must fall through to the direct launch
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    if ! systemctl is-active --quiet spdk-nvmf-tgt; then
        cat >/etc/systemd/system/spdk-nvmf-tgt.service <<EOF
[Unit]
Description=SPDK NVMe-oF target
After=network.target

[Service]
Type=simple
ExecStart=${NVMF_TGT} ${EAL_CORES} -s 2048
Restart=always

[Install]
WantedBy=multi-user.target
EOF
        systemctl daemon-reload
        systemctl enable --now spdk-nvmf-tgt
    fi
elif [ -S "${RPC_SOCK}" ] && $RPC spdk_get_version >/dev/null 2>&1; then
    echo "nvmf_tgt rpc socket ${RPC_SOCK} alive, reusing the running target"
else
    # a leftover socket file without a listener (the process died without unlinking
    # it) must not be mistaken for a running target: probe, remove, relaunch
    if [ -S "${RPC_SOCK}" ]; then
        echo "stale rpc socket ${RPC_SOCK} (no listener), removing and relaunching"
        rm -f "${RPC_SOCK}" "${RPC_SOCK}.lock"
    fi
    start_tgt_direct
fi

# 2) wait for the rpc socket
for _ in $(seq 1 30); do [ -S "${RPC_SOCK}" ] && break; sleep 1; done
[ -S "${RPC_SOCK}" ] || { echo "nvmf_tgt rpc socket ${RPC_SOCK} never appeared (see /tmp/nvmf_tgt.log)" >&2; exit 1; }

# 3) backing bdev: simulated malloc bdev, or the local NVMe drives (multi-drive: raid0 bdev)
if [ "${SIM}" = 1 ]; then
    $RPC bdev_malloc_delete "${BASE_BDEV}" >/dev/null 2>&1 || true
    $RPC bdev_malloc_create -b "${BASE_BDEV}" "${SIM_SIZE_MB}" 512
else
    $RPC bdev_nvme_attach_controller -b Nvme0 -t PCIe -a 0000:11:00.0 || true
    #    aggregation example for >=2 drives:
    #    $RPC bdev_nvme_attach_controller -b Nvme1 -t PCIe -a 0000:12:00.0
    #    $RPC bdev_raid_create -z raid0 -b "${BASE_BDEV}" -b Nvme1n1 -r 128k
fi

# 4) transport + subsystem + listener; any-host auth (pool isolation is the
#    volatile ns lifecycle, host-nqn ACLs can be tightened later).
#    NOTE: this SPDK generation takes the NQN as a positional argument, and
#    allow_any_host defaults to enabled (--allow/--enable do not exist)
if [ "${TRANSPORT}" = "RDMA" ]; then
    $RPC nvmf_create_transport -t RDMA -u 8192 -m 8 || true
else
    $RPC nvmf_create_transport -t TCP || true
fi
$RPC nvmf_create_subsystem "${NQN}" || true
$RPC nvmf_subsystem_add_listener -t "${TRANSPORT}" -a "${TRADDR}" -s "${TRSVCID}" -f IPv4 "${NQN}" || true
$RPC nvmf_subsystem_allow_any_host "${NQN}" || true

# 5) hard verification: the || true above must not mask a dead target
SUBSYS_JSON=$($RPC nvmf_get_subsystems)
echo "${SUBSYS_JSON}" | grep -q "\"${NQN}\"" \
    || { echo "subsystem ${NQN} not registered - target NOT functional" >&2; exit 1; }
echo "${SUBSYS_JSON}" | grep -q "\"${TRADDR}\"" \
    || { echo "listener ${TRADDR}:${TRSVCID} not registered - target NOT functional" >&2; exit 1; }

echo "nvmf target up (transport=${TRANSPORT}, sim=${SIM}):"
echo "  nqn        = ${NQN}"
echo "  listener   = ${TRADDR}:${TRSVCID}"
echo "  base bdev  = ${BASE_BDEV} (extents are carved as uniform splits by the daemon)"
if [ "${SIM}" = 1 ]; then
    echo "daemon env  : MF_NVME_NQN=${NQN} MF_NVME_TRADDR=${TRADDR} MF_NVME_BASE_BDEV=${BASE_BDEV}"
    if [ "${TRANSPORT}" = "RDMA" ]; then
        echo "client op   : data_op_type = NVME_RDMA (simulated disk, real RDMA listener)"
    else
        echo "client op   : data_op_type = NVME_TCP (single media; FAR verifies the TCP listener matches)"
    fi
else
    echo "daemon env  : MF_NVME_NQN=${NQN} MF_NVME_TRADDR=${TRADDR} MF_NVME_BASE_BDEV=${BASE_BDEV}"
    echo "client op   : data_op_type = <host op> | NVME_RDMA (FAR verifies the RDMA listener matches)"
fi
if [ -n "${SPDK_ROOT}" ]; then
    # --with-shared builds also need the DPDK libs from <spdk>/dpdk/build/lib
    echo "initiator   : export LD_LIBRARY_PATH=${SPDK_ROOT}/dpdk/build/lib:${SPDK_ROOT}/build/lib:\${LD_LIBRARY_PATH}"
fi