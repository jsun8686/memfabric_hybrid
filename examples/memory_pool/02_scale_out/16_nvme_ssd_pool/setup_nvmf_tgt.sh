#!/usr/bin/env bash
# NVMe-oF target bootstrap for the memfabric NVMe pool (FAR node, one-time).
# Loads SPDK's nvmf_tgt as a systemd service and exposes the local NVMe bdevs as
# an RDMA subsystem. The memfabric daemon only signs LBA extents (namespaces) over
# the local JSON-RPC socket; identity env for the daemon:
#   export MF_NVME_NQN=<subsystem nqn printed below>
#   export MF_NVME_TRADDR=<this node's storage-plane IP>
#   export MF_NVME_TRSVCID=4420            # default
#   export MF_NVME_RPC_SOCK=/var/tmp/spdk.sock
#   export MF_NVME_BASE_BDEV=Nvme0n1      # bdev backing the pool (raided bdev for multi-drive)
#   export MF_NVME_LBA_BASE=0             # disjoint nonzero bases when several FAR ranks share one node
# Usage: sudo ./setup_nvmf_tgt.sh <traddr> [bdev]
set -euo pipefail

TRADDR="${1:?usage: $0 <traddr> [bdev]}"
BASE_BDEV="${2:-Nvme0n1}"
TRSVCID=4420
NQN="nqn.2016-06.io.spdk:memfabric"
RPC_SOCK=/var/tmp/spdk.sock
RPC="python3 $(command -v spdk_rpc 2>/dev/null || echo rpc.py) -s ${RPC_SOCK}"

# 1) nvmf_tgt as a persistent service (assumes an SPDK build tree or installed package)
if ! systemctl is-active --quiet spdk-nvmf-tgt; then
    cat >/etc/systemd/system/spdk-nvmf-tgt.service <<EOF
[Unit]
Description=SPDK NVMe-oF target
After=network.target

[Service]
Type=simple
ExecStart=/usr/libexec/spdk/nvmf_tgt -m 0x3 -s 2048
Restart=always

[Install]
WantedBy=multi-user.target
EOF
    systemctl daemon-reload
    systemctl enable --now spdk-nvmf-tgt
fi

# 2) wait for the rpc socket
for _ in $(seq 1 30); do [ -S "${RPC_SOCK}" ] && break; sleep 1; done
[ -S "${RPC_SOCK}" ] || { echo "nvmf_tgt rpc socket ${RPC_SOCK} never appeared" >&2; exit 1; }

# 3) attach the local NVMe drives (multi-drive: raid0 bdev named the same below)
$RPC bdev_nvme_attach_controller -b Nvme0 -t PCIe -a 0000:11:00.0 || true
#    aggregation example for >=2 drives:
#    $RPC bdev_nvme_attach_controller -b Nvme1 -t PCIe -a 0000:12:00.0
#    $RPC bdev_raid_create -z raid0 -b "${BASE_BDEV}" -b Nvme1n1 -r 128k

# 4) RDMA transport + subsystem + listener; any-host auth (pool isolation is the
#    volatile ns lifecycle, host-nqn ACLs can be tightened later)
$RPC nvmf_create_transport -t RDMA -u 8192 -m 8
$RPC nvmf_create_subsystem -s "${NQN}" || true
$RPC nvmf_subsystem_add_listener -t RDMA -a "${TRADDR}" -s "${TRSVCID}" -f IPv4 "${NQN}" || true
$RPC nvmf_subsystem_allow_any_host "${NQN}" --allow || true

echo "nvmf target up:"
echo "  nqn        = ${NQN}"
echo "  listener   = ${TRADDR}:${TRSVCID}"
echo "  base bdev  = ${BASE_BDEV} (extents are carved as bdev_split + linear runs by the daemon)"
echo "daemon env  : MF_NVME_NQN=${NQN} MF_NVME_TRADDR=${TRADDR} MF_NVME_BASE_BDEV=${BASE_BDEV}"
