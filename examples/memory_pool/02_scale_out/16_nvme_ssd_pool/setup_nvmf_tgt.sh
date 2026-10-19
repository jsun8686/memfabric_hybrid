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
#   - initiator side must then run with: export MF_NVME_TRSTRING=tcp
#
# SPDK_ROOT (optional): points at an SPDK source tree -- the script then uses
#   ${SPDK_ROOT}/build/bin/nvmf_tgt and ${SPDK_ROOT}/scripts/rpc.py directly and
#   prints the LD_LIBRARY_PATH the NEAR side needs for dlopen. Without SPDK_ROOT
#   a package install layout is assumed (systemd unit + rpc.py on PATH).
#
# Usage: sudo [SPDK_ROOT=...] bash ./setup_nvmf_tgt.sh <traddr> [bdev] [--sim]
# (bash is required; invoking via sh re-execs bash automatically)

# re-exec under bash when mistakenly invoked via sh/dash (dash lacks pipefail)
[ -z "${BASH_VERSION:-}" ] && exec bash "$0" "$@"
set -eu

TRADDR=""
BASE_BDEV=""
SIM=0
for arg in "$@"; do
    case "${arg}" in
        --sim) SIM=1 ;;
        *) if [ -z "${TRADDR}" ]; then TRADDR="${arg}"; else BASE_BDEV="${arg}"; fi ;;
    esac
done
: "${TRADDR:?usage: $0 <traddr> [bdev] [--sim]}"

if [ -z "${BASE_BDEV}" ]; then
    # sim default is the malloc bdev created below, real default is the first NVMe drive
    if [ "${SIM}" = 1 ]; then BASE_BDEV="Malloc0"; else BASE_BDEV="Nvme0n1"; fi
fi
TRSVCID=4420
NQN="nqn.2016-06.io.spdk:memfabric"
SIM_SIZE_MB="${SIM_SIZE_MB:-1024}"
TRANSPORT="RDMA"
if [ "${SIM}" = 1 ]; then TRANSPORT="TCP"; fi
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

# 0) simulation needs DPDK hugepages for the malloc bdev (4G of 2M pages, no-op if already there)
if [ "${SIM}" = 1 ]; then
    HUGEPD=/sys/kernel/mm/hugepages/hugepages-2048kB
    WANT=2048
    HAVE=$(cat "${HUGEPD}/nr_hugepages")
    if [ "${HAVE}" -lt "${WANT}" ]; then
        echo "${WANT}" > "${HUGEPD}/nr_hugepages" || {
            echo "cannot raise hugepages to ${WANT} (have ${HAVE}); free memory or set SIM_SIZE_MB smaller" >&2
            exit 1
        }
    fi
fi

# 1) target daemon: systemd service when available, direct nohup launch otherwise
#    (containers usually have no systemd as PID 1)
if command -v systemctl >/dev/null 2>&1; then
    if ! systemctl is-active --quiet spdk-nvmf-tgt; then
        cat >/etc/systemd/system/spdk-nvmf-tgt.service <<EOF
[Unit]
Description=SPDK NVMe-oF target
After=network.target

[Service]
Type=simple
ExecStart=${NVMF_TGT} -m 0x3 -s 2048
Restart=always

[Install]
WantedBy=multi-user.target
EOF
        systemctl daemon-reload
        systemctl enable --now spdk-nvmf-tgt
    fi
elif [ -S "${RPC_SOCK}" ]; then
    echo "nvmf_tgt rpc socket ${RPC_SOCK} already exists, reusing the running target"
else
    nohup "${NVMF_TGT}" -m 0x3 -s 2048 -r "${RPC_SOCK}" >/tmp/nvmf_tgt.log 2>&1 &
    echo "nvmf_tgt started directly (no systemd in this environment)"
    echo "  log: /tmp/nvmf_tgt.log   stop: pkill nvmf_tgt"
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
# volatile ns lifecycle, host-nqn ACLs can be tightened later)
if [ "${TRANSPORT}" = "RDMA" ]; then
    $RPC nvmf_create_transport -t RDMA -u 8192 -m 8
else
    $RPC nvmf_create_transport -t TCP
fi
$RPC nvmf_create_subsystem -n "${NQN}" || true
$RPC nvmf_subsystem_add_listener -t "${TRANSPORT}" -a "${TRADDR}" -s "${TRSVCID}" -f IPv4 "${NQN}" || true
$RPC nvmf_subsystem_allow_any_host "${NQN}" --allow || true

echo "nvmf target up (transport=${TRANSPORT}, sim=${SIM}):"
echo "  nqn        = ${NQN}"
echo "  listener   = ${TRADDR}:${TRSVCID}"
echo "  base bdev  = ${BASE_BDEV} (extents are carved as bdev_split + linear runs by the daemon)"
if [ "${SIM}" = 1 ]; then
    echo "daemon env  : MF_NVME_NQN=${NQN} MF_NVME_TRADDR=${TRADDR} MF_NVME_BASE_BDEV=${BASE_BDEV} MF_NVME_TRSTRING=tcp"
else
    echo "daemon env  : MF_NVME_NQN=${NQN} MF_NVME_TRADDR=${TRADDR} MF_NVME_BASE_BDEV=${BASE_BDEV}"
fi
if [ -n "${SPDK_ROOT}" ]; then
    echo "initiator   : export LD_LIBRARY_PATH=${SPDK_ROOT}/build/lib:\${LD_LIBRARY_PATH} (NEAR side dlopen)"
fi
