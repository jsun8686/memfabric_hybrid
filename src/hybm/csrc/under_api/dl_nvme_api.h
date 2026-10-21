/*
 * Copyright: (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#ifndef MF_HYBRID_DL_NVME_API_H
#define MF_HYBRID_DL_NVME_API_H

#include "hybm_common_include.h"

namespace ock {
namespace mf {

/* Minimal self-declared SPDK ABI subset -- the build never requires the SPDK SDK, the
 * symbols are resolved with dlopen at runtime. Layout below mirrors upstream
 * include/spdk/nvme.h struct spdk_nvme_transport_id exactly: field ORDER matters
 * (trtype/adrfam are 4-byte enums placed before traddr), canonical constants:
 * SPDK_NVMF_TRSTRING_MAX_LEN=32 / TRADDR=256 / TRSVCID=32 / NQN=256, arrays +1. */
struct MfSpdkNvmeTransportId {
    char trstring[32 + 1];  /* transport name: "rdma" (default) or "tcp" (disk-less simulation) */
    uint32_t trtype;        /* enum spdk_nvme_transport_type: NVMe-oF spec value, 8-bit range */
    uint32_t adrfam;        /* enum spdk_nvmf_adrfam: 0 = INVALID, 1 = IPv4, 2 = IPv6 */
    char traddr[256 + 1];   /* target IP string */
    char trsvcid[32 + 1];   /* target port string */
    char subnqn[256 + 1];   /* subsystem NQN */
    int32_t priority;       /* not used */
};
static_assert(sizeof(MfSpdkNvmeTransportId) == 596, "must match sizeof(struct spdk_nvme_transport_id)");

/* trtype values of enum spdk_nvme_transport_type for fabrics transports (NVMe
 * spec: 1=RDMA, 2=FC, 3=TCP; transport ROUTING is by trstring, but trtype is
 * decoded in logs and checked by spdk internals -- both fields must be set) */
enum MfSpdkNvmeTrtype {
    MF_SPDK_NVME_TRTYPE_RDMA = 1,
    MF_SPDK_NVME_TRTYPE_FC = 2,
    MF_SPDK_NVME_TRTYPE_TCP = 3,
};

/* mirrors spdk_nvmf_adrfam: 0 = INVALID, 1 = IPv4, 2 = IPv6 (the TCP/RDMA clients
 * reject an INVALID adrfam with "Unhandled ADRFAM 0", it must be set explicitly) */
enum MfSpdkNvmfAdrfam {
    MF_SPDK_NVMF_ADRFAM_INVALID = 0,
    MF_SPDK_NVMF_ADRFAM_IPV4 = 1,
    MF_SPDK_NVMF_ADRFAM_IPV6 = 2,
};

/* Leading fields of upstream struct spdk_nvme_ctrlr_opts (v25.05 include/spdk/nvme.h,
 * field order verified). OFFSET USE ONLY -- spdk_nvme_connect()'s opts_size MUST be
 * sizeof(struct spdk_nvme_ctrlr_opts) (or 0 with opts==NULL): the library only
 * default-fills/copies within opts_size and leaves the rest of its internal opts
 * UNINITIALIZED, so a short prefix here feeds garbage pointers (tls_psk/dhchap)
 * into the fabric connect and crashes. */
struct MfSpdkNvmeCtrlrOptsPrefix {
    uint32_t numIoQueues;          /* 0 = driver default */
    bool useCmbSqs;
    bool noShnNotification;
    bool enableInterrupts;
    uint8_t reserved7;             /* upstream hole at byte 7 */
    uint32_t arbMechanism;         /* enum spdk_nvme_cc_ams */
    uint8_t arbitrationBurst;
    uint8_t lowPriorityWeight;
    uint8_t mediumPriorityWeight;
    uint8_t highPriorityWeight;
    uint32_t keepAliveTimeoutMs;   /* 0 = disabled */
};
static_assert(sizeof(MfSpdkNvmeCtrlrOptsPrefix) == 20, "must match the upstream opts prefix layout");

/* mirrors upstream SPDK_STATIC_ASSERT(sizeof(struct spdk_nvme_ctrlr_opts) == 856);
 * the buffer is opaque below -- field access goes through the prefix offsets only */
static constexpr size_t MF_NVME_CTRLR_OPTS_SIZE = 856;
static_assert(MF_NVME_CTRLR_OPTS_SIZE % alignof(uint64_t) == 0, "keeps uint64 fields aligned");

/* callback of an NVMe IO command; cpl points to struct spdk_nvme_cpl (opaque here) */
using MfSpdkNvmeCmdCb = void (*)(void *ctx, const void *cpl);

using SpdkEnvDpdkPostInitFunc = int (*)(bool legacyMem);
using SpdkDmaMallocFunc = void *(*)(size_t size, size_t align, uint64_t *physAddr);
using SpdkDmaFreeFunc = void (*)(void *buf);
/* v25.05 ABI: spdk_nvme_connect carries (opts, opts_size) -- opts==NULL with
 * opts_size==0 selects the default options (no TLS/PSK keys) */
using SpdkNvmeConnectFunc = void *(*)(const MfSpdkNvmeTransportId *trid, const void *opts, size_t optsSize);
using SpdkNvmeGetDefaultCtrlrOptsFunc = void (*)(void *opts, size_t optsSize);
using SpdkNvmeDetachFunc = int (*)(void *ctrlr);
using SpdkNvmeCtrlrGetNsFunc = void *(*)(void *ctrlr, uint32_t nsid);
using SpdkNvmeCtrlrAllocIoQpairFunc = void *(*)(void *ctrlr, const void *opts, size_t optsSize);
using SpdkNvmeCtrlrFreeIoQpairFunc = int (*)(void *qpair);
using SpdkNvmeCtrlrGetMaxXferSizeFunc = uint32_t (*)(void *ctrlr);
using SpdkNvmeNsCmdReadFunc = int (*)(void *ns, void *qpair, void *buf, uint64_t lba, uint32_t lbaCount,
                                      MfSpdkNvmeCmdCb cb, void *ctx, uint32_t ioFlags);
using SpdkNvmeNsCmdWriteFunc = int (*)(void *ns, void *qpair, void *buf, uint64_t lba, uint32_t lbaCount,
                                       MfSpdkNvmeCmdCb cb, void *ctx, uint32_t ioFlags);
using SpdkNvmeQpairProcessCompletionsFunc = int32_t (*)(void *qpair, uint32_t maxCompletions);

/* dlopen loader of the SPDK initiator libraries (env + nvme). Absence is NOT fatal for
 * the process: loading is lazy (first NVMe IO), pools without the NVMe op bit never
 * touch it. UT replaces the public static function pointers with stubs. */
class DlNvmeApi {
public:
    static Result LoadLibrary();
    static void CleanupLibrary();

    /* bring the DPDK EAL up inside the host process (external-init mode):
     * rte_eal_init("--in-memory") -- no hugepages, no shared config files; safe to
     * call repeatedly, absent symbols fall through to BM_NOT_SUPPORTED */
    static Result InitDpdkEnv();

    /* the SPDK env may also be initialized externally (e.g. embedded in the caller);
     * the post-init call is optional, a missing symbol is not an error */
    static inline int EnvDpdkPostInit(bool legacyMem)
    {
        if (gSpdkEnvDpdkPostInit == nullptr) {
            return -1;
        }
        return gSpdkEnvDpdkPostInit(legacyMem);
    }

    static inline Result DmaMalloc(size_t size, size_t align, void *&buf)
    {
        if (gSpdkDmaMalloc == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        buf = gSpdkDmaMalloc(size, align, nullptr);
        return buf != nullptr ? BM_OK : BM_MALLOC_FAILED;
    }

    static inline void DmaFree(void *buf)
    {
        if (gSpdkDmaFree != nullptr && buf != nullptr) {
            gSpdkDmaFree(buf);
        }
    }

    static inline Result Connect(MfSpdkNvmeTransportId &trid, void *&ctrlr)
    {
        if (gSpdkNvmeConnect == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        /* keep-alive MUST be disabled: this host never polls the admin qpair, so the
         * library would never submit KAT commands and the target disconnects us after
         * the default 10s timeout (verified: nvmf_ctrlr_keep_alive_poll disconnect).
         * opts_size must be the FULL upstream size -- see MfSpdkNvmeCtrlrOptsPrefix. */
        if (gSpdkNvmeGetDefaultCtrlrOpts != nullptr) {
            alignas(8) uint8_t optsBuf[MF_NVME_CTRLR_OPTS_SIZE];
            gSpdkNvmeGetDefaultCtrlrOpts(optsBuf, sizeof(optsBuf));
            auto *opts = reinterpret_cast<MfSpdkNvmeCtrlrOptsPrefix *>(optsBuf);
            opts->keepAliveTimeoutMs = 0;
            ctrlr = gSpdkNvmeConnect(&trid, optsBuf, sizeof(optsBuf));
        } else {
            /* symbol absent (exotic build): fall back to library defaults -- accepts
             * the default KAT rather than risking an undersized opts */
            ctrlr = gSpdkNvmeConnect(&trid, nullptr, 0);
        }
        return ctrlr != nullptr ? BM_OK : BM_ERROR;
    }

    static inline void Detach(void *ctrlr)
    {
        if (gSpdkNvmeDetach != nullptr && ctrlr != nullptr) {
            (void)gSpdkNvmeDetach(ctrlr);
        }
    }

    static inline Result GetNs(void *ctrlr, uint32_t nsid, void *&ns)
    {
        if (gSpdkNvmeCtrlrGetNs == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        ns = gSpdkNvmeCtrlrGetNs(ctrlr, nsid);
        return ns != nullptr ? BM_OK : BM_ERROR;
    }

    static inline Result AllocIoQpair(void *ctrlr, void *&qpair)
    {
        if (gSpdkNvmeCtrlrAllocIoQpair == nullptr) {
            return BM_UNDER_API_UNLOAD;
        }
        /* opts = nullptr -> driver defaults, optsSize 0 */
        qpair = gSpdkNvmeCtrlrAllocIoQpair(ctrlr, nullptr, 0);
        return qpair != nullptr ? BM_OK : BM_ERROR;
    }

    static inline void FreeIoQpair(void *qpair)
    {
        if (gSpdkNvmeCtrlrFreeIoQpair != nullptr && qpair != nullptr) {
            (void)gSpdkNvmeCtrlrFreeIoQpair(qpair);
        }
    }

    /* controller MDTS (max bytes per IO command); 0 when unknown -> caller clamps */
    static inline uint32_t MaxXferSize(void *ctrlr)
    {
        if (gSpdkNvmeCtrlrGetMaxXferSize == nullptr || ctrlr == nullptr) {
            return 0;
        }
        return gSpdkNvmeCtrlrGetMaxXferSize(ctrlr);
    }

    static inline int NsCmdRead(void *ns, void *qpair, void *buf, uint64_t lba, uint32_t lbaCount,
                                MfSpdkNvmeCmdCb cb, void *ctx)
    {
        if (gSpdkNvmeNsCmdRead == nullptr) {
            return -1;
        }
        return gSpdkNvmeNsCmdRead(ns, qpair, buf, lba, lbaCount, cb, ctx, 0);
    }

    static inline int NsCmdWrite(void *ns, void *qpair, void *buf, uint64_t lba, uint32_t lbaCount,
                                 MfSpdkNvmeCmdCb cb, void *ctx)
    {
        if (gSpdkNvmeNsCmdWrite == nullptr) {
            return -1;
        }
        return gSpdkNvmeNsCmdWrite(ns, qpair, buf, lba, lbaCount, cb, ctx, 0);
    }

    static inline int32_t ProcessCompletions(void *qpair)
    {
        if (gSpdkNvmeQpairProcessCompletions == nullptr) {
            return -1;
        }
        /* max = 0 -> process all completions currently available */
        return gSpdkNvmeQpairProcessCompletions(qpair, 0);
    }

    /* public for UT stubbing (same pattern as DlHccpApi) */
    static SpdkEnvDpdkPostInitFunc gSpdkEnvDpdkPostInit;
    static SpdkDmaMallocFunc gSpdkDmaMalloc;
    static SpdkDmaFreeFunc gSpdkDmaFree;
    static SpdkNvmeConnectFunc gSpdkNvmeConnect;
    static SpdkNvmeGetDefaultCtrlrOptsFunc gSpdkNvmeGetDefaultCtrlrOpts;
    static SpdkNvmeDetachFunc gSpdkNvmeDetach;
    static SpdkNvmeCtrlrGetNsFunc gSpdkNvmeCtrlrGetNs;
    static SpdkNvmeCtrlrAllocIoQpairFunc gSpdkNvmeCtrlrAllocIoQpair;
    static SpdkNvmeCtrlrFreeIoQpairFunc gSpdkNvmeCtrlrFreeIoQpair;
    static SpdkNvmeCtrlrGetMaxXferSizeFunc gSpdkNvmeCtrlrGetMaxXferSize;
    static SpdkNvmeNsCmdReadFunc gSpdkNvmeNsCmdRead;
    static SpdkNvmeNsCmdWriteFunc gSpdkNvmeNsCmdWrite;
    static SpdkNvmeQpairProcessCompletionsFunc gSpdkNvmeQpairProcessCompletions;

private:
    static bool gLoaded;
    static std::mutex gMutex;
    static void *gEnvHandle;
    static void *gNvmeHandle;
};

} // namespace mf
} // namespace ock

#endif // MF_HYBRID_DL_NVME_API_H
