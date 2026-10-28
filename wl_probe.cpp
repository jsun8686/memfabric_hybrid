/*
 * wl_probe.cpp -- standalone probe for the libra white-list switch.
 *
 * Question to answer: does RaSocketSetWhiteListStatus(0) still take effect when
 * called AFTER RaInit (and after ListenStart)?  This decides where the product
 * integration point can live.
 *
 *   server:  ./wl_probe server  <devid> <localRoceIp> <port> <mode>
 *   client:  ./wl_probe client  <devid> <localRoceIp> <serverRoceIp> <port> <mode>
 *
 *   mode:
 *     default    -- never touch the switch (control run: expect REJECTED,
 *                   proving the probe itself is meaningful)
 *     off-before -- RaSocketSetWhiteListStatus(0) BEFORE anything (TsdOpen/RaInit/...)
 *     off-after  -- RaSocketSetWhiteListStatus(0) AFTER the full device prepare
 *                   (TsdOpen + RaInit + RaRdevInitV2), BEFORE ListenStart
 *
 * Verdict matrix (server never adds any white-list entry):
 *   default    rejected  -> probe valid
 *   off-before accepted  -> consistent with "must be before RaInit"
 *   off-after  accepted  -> switch is live post-init: integration point can be
 *                           anywhere before the first connect (product-friendly)
 *   off-after  rejected  -> strict pre-RaInit constraint: on FSDP NEAR nodes
 *                           torch_npu/HCCL has already RaInit-ed, needs more study
 *
 * Build (on the cluster, aarch64):
 *   g++ -O0 -g -std=c++11 wl_probe.cpp -o wl_probe -ldl
 *
 * Prefer CROSS-NODE runs (one NEAR host + one FAR host): same-host connects ride
 * the loopback route and the kernel may rewrite the source address, which has
 * caused misleading results before.
 */
#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <chrono>
#include <string>

/* ---- minimal copies of the libra ABI types (kept byte-identical with
 *      src/hybm/csrc/under_api/dl_hccp_def.h) ---- */
constexpr uint32_t HCCP_SOCK_CONN_TAG_SIZE = 192;

union HccpIpAddrT {
    struct in_addr addr;
    struct in6_addr addr6;
};

struct HccpRaInitConfigT {
    uint32_t phyId;
    uint32_t nicPosition; /* NETWORK_OFFLINE == 1 */
    int hdcType;          /* 6 = HDC_SERVICE_TYPE_RDMA, same as the product */
    bool enableHdcAsync;
};

struct HccpRdevT {
    uint32_t phyId;
    int family; /* AF_INET */
    HccpIpAddrT localIp;
};

struct HccpRdevInitInfoT {
    int mode;               /* NETWORK_OFFLINE == 1 */
    uint32_t notifyType;    /* NOTIFY == 1 */
    bool enabled910aLite;
    bool disabledLiteThread;
    bool enabled2mbLite;    /* product sets true: support 64k os */
};

struct HccpSocketListenInfoT {
    void *handle;
    unsigned int port;
    unsigned int phase;
    unsigned int err;
};

struct HccpSocketConnectInfoT {
    void *handle;
    HccpIpAddrT remoteIp;
    uint32_t port;
    char tag[HCCP_SOCK_CONN_TAG_SIZE];
};

struct HccpSocketInfoT {
    void *handle;
    void *fd;
    HccpIpAddrT remoteIp;
    int status; /* 0 not connected 1 connected 2 timeout 3 connecting */
    char tag[HCCP_SOCK_CONN_TAG_SIZE];
};

/* ---- libra symbols ---- */
static void *gRa = nullptr;
static int (*gRaInit)(const HccpRaInitConfigT *) = nullptr;
static int (*gRaDeinit)(void) = nullptr;
static int (*gRaSocketInit)(int, HccpRdevT, void **) = nullptr;
static int (*gRaSocketDeinit)(void *) = nullptr;
static int (*gRaSocketListenStart)(HccpSocketListenInfoT *, unsigned int) = nullptr;
static int (*gRaSocketListenStop)(HccpSocketListenInfoT *, unsigned int) = nullptr;
static int (*gRaSocketBatchConnect)(HccpSocketConnectInfoT *, uint32_t) = nullptr;
static int (*gRaGetSockets)(uint32_t, HccpSocketInfoT *, uint32_t, uint32_t *) = nullptr;
static int (*gRaSetWhiteListStatus)(uint32_t) = nullptr;
static int (*gRaGetWhiteListStatus)(uint32_t *) = nullptr;
static uint32_t (*gTsdOpen)(uint32_t, uint32_t) = nullptr;
static int (*gRaRdevGetHandle)(uint32_t, void **) = nullptr;
static int (*gRaRdevInitV2)(HccpRdevInitInfoT, HccpRdevT, void **) = nullptr;

static void Die(const char *msg)
{
    fprintf(stderr, "FATAL: %s\n", msg);
    exit(1);
}

static void *Sym(const char *name, const char *altName)
{
    void *p = dlsym(gRa, name);
    if (p == nullptr && altName != nullptr) {
        p = dlsym(gRa, altName);
    }
    return p;
}

static void LoadLibra()
{
    const char *lib = getenv("WL_LIB");
    if (lib == nullptr || lib[0] == '\0') {
        lib = "/usr/local/Ascend/cann-9.0.0/aarch64-linux/lib64/libra.so";
    }
    gRa = dlopen(lib, RTLD_NOW);
    if (gRa == nullptr) {
        fprintf(stderr, "FATAL: dlopen(%s) failed: %s\n", lib, dlerror());
        exit(1);
    }
    /* every symbol also has a lowercase snake_case twin in libra; try both */
    gRaInit = (int (*)(const HccpRaInitConfigT *))Sym("RaInit", "ra_init");
    gRaDeinit = (int (*)(void))Sym("RaDeinit", "ra_deinit");
    gRaSocketInit = (int (*)(int, HccpRdevT, void **))Sym("RaSocketInit", "ra_socket_init");
    gRaSocketDeinit = (int (*)(void *))Sym("RaSocketDeinit", "ra_socket_deinit");
    gRaSocketListenStart =
        (int (*)(HccpSocketListenInfoT *, unsigned int))Sym("RaSocketListenStart", "ra_socket_listen_start");
    gRaSocketListenStop =
        (int (*)(HccpSocketListenInfoT *, unsigned int))Sym("RaSocketListenStop", "ra_socket_listen_stop");
    gRaSocketBatchConnect =
        (int (*)(HccpSocketConnectInfoT *, uint32_t))Sym("RaSocketBatchConnect", "ra_socket_batch_connect");
    gRaGetSockets =
        (int (*)(uint32_t, HccpSocketInfoT *, uint32_t, uint32_t *))Sym("RaGetSockets", "ra_get_sockets");
    gRaSetWhiteListStatus = (int (*)(uint32_t))Sym("RaSocketSetWhiteListStatus", "ra_socket_set_white_list_status");
    gRaGetWhiteListStatus = (int (*)(uint32_t *))Sym("RaSocketGetWhiteListStatus", "ra_socket_get_white_list_status");
    gTsdOpen = (uint32_t (*)(uint32_t, uint32_t))Sym("TsdOpen", "tsd_open");
    gRaRdevGetHandle = (int (*)(uint32_t, void **))Sym("RaRdevGetHandle", "ra_rdev_get_handle");
    gRaRdevInitV2 = (int (*)(HccpRdevInitInfoT, HccpRdevT, void **))Sym("RaRdevInitV2", "ra_rdev_init_v2");

    if (gRaInit == nullptr || gRaSocketInit == nullptr || gRaSocketListenStart == nullptr ||
        gRaSocketBatchConnect == nullptr || gRaGetSockets == nullptr || gTsdOpen == nullptr ||
        gRaRdevInitV2 == nullptr) {
        Die("required socket symbols missing in libra");
    }
    printf("[probe] libra loaded: %s\n", lib);
    printf("[probe] RaSocketSetWhiteListStatus: %s  RaSocketGetWhiteListStatus: %s\n",
           gRaSetWhiteListStatus != nullptr ? "FOUND" : "MISSING",
           gRaGetWhiteListStatus != nullptr ? "FOUND" : "MISSING");
}

static void ReportSwitch(const char *when)
{
    if (gRaGetWhiteListStatus == nullptr) {
        printf("[switch:%s] GetWhiteListStatus not available\n", when);
        return;
    }
    uint32_t enable = 0xDEADBEEFU;
    auto ret = gRaGetWhiteListStatus(&enable);
    printf("[switch:%s] GetWhiteListStatus ret=%d enable=%u\n", when, ret, enable);
}

static void SwitchOff(const char *when)
{
    if (gRaSetWhiteListStatus == nullptr) {
        Die("RaSocketSetWhiteListStatus missing in this libra");
    }
    auto ret = gRaSetWhiteListStatus(0);
    printf("[switch:%s] SetWhiteListStatus(0) ret=%d\n", when, ret);
    ReportSwitch(when);
}

static void DoPrepareDevice(uint32_t devid, const char *localIp)
{
    /* full product sequence (RdmaTransportManager::PrepareOpenDevice):
     * TsdOpen -> RaInit -> RaRdevInitV2. Skipping TsdOpen was why the first
     * probe run died with RaInit ret=128000. */
    auto tsd = gTsdOpen(devid, 2 /* rankCount: client+server world */);
    printf("[prepare] TsdOpen(devid=%u, rankSize=2) ret=%u\n", devid, tsd);

    bool raOk = false;
    for (int attempt = 1; attempt <= 2 && !raOk; attempt++) {
        std::this_thread::sleep_for(std::chrono::seconds(3)); /* same as product: avoid hccl conflict */
        HccpRaInitConfigT cfg{};
        cfg.phyId = devid;
        cfg.nicPosition = 1; /* NETWORK_OFFLINE */
        cfg.hdcType = 6;     /* HDC_SERVICE_TYPE_RDMA */
        cfg.enableHdcAsync = false;
        auto ret = gRaInit(&cfg);
        printf("[prepare] RaInit attempt %d ret=%d (phyId=%u)\n", attempt, ret, devid);
        raOk = (ret == 0);
    }
    if (!raOk) {
        /* 328000-range: another RA user (leftover daemon / HCCL) holds the device --
         * the product would fall back to an hccl-owned handle; the probe has none, but
         * RaRdevInitV2 below may still succeed, so continue and report */
        printf("[prepare] NOTE: RaInit failed, continuing to RaRdevInitV2 (pkill leftover memfabric/HCCL users?)\n");
    }

    HccpRdevInitInfoT info{};
    info.mode = 1;               /* NETWORK_OFFLINE */
    info.notifyType = 1;         /* NOTIFY */
    info.enabled910aLite = false;
    info.disabledLiteThread = false;
    info.enabled2mbLite = true;  /* same as product */
    HccpRdevT rdev{};
    rdev.phyId = devid;
    rdev.family = AF_INET;
    if (inet_pton(AF_INET, localIp, &rdev.localIp.addr) != 1) {
        Die("invalid local RoCE ip");
    }
    void *rdmaHandle = nullptr;
    if (gRaRdevGetHandle != nullptr) {
        auto reuse = gRaRdevGetHandle(devid, &rdmaHandle);
        printf("[prepare] RaRdevGetHandle ret=%d handle=%p\n", reuse, rdmaHandle);
    }
    if (rdmaHandle == nullptr) {
        auto ret = gRaRdevInitV2(info, rdev, &rdmaHandle);
        printf("[prepare] RaRdevInitV2 ret=%d handle=%p\n", ret, rdmaHandle);
        if (ret != 0) {
            Die("RaRdevInitV2 failed (leftover daemon/HCCL holding the device? pkill them first)");
        }
    }
}

static void *MakeSocket(uint32_t devid, const char *localIp)
{
    HccpRdevT rdev{};
    rdev.phyId = devid;
    rdev.family = AF_INET;
    if (inet_pton(AF_INET, localIp, &rdev.localIp.addr) != 1) {
        Die("invalid local RoCE ip");
    }
    void *sock = nullptr;
    auto ret = gRaSocketInit(1 /* NETWORK_OFFLINE */, rdev, &sock); /* 1 == NETWORK_OFFLINE */
    if (ret != 0 || sock == nullptr) {
        fprintf(stderr, "FATAL: RaSocketInit failed ret=%d\n", ret);
        exit(1);
    }
    return sock;
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr,
                "usage:\n"
                "  server: %s server <devid> <localRoceIp> <port> <default|off-before|off-after>\n"
                "  client: %s client <devid> <localRoceIp> <serverRoceIp> <port> <mode>\n",
                argv[0], argv[0]);
        return 1;
    }
    std::string role = argv[1];
    uint32_t devid = (uint32_t)atoi(argv[2]);
    const char *localIp = argv[3];
    /* argv[4]: server->port, client->serverRoceIp */
    const char *mode = argv[argc - 1];
    bool offBefore = (strcmp(mode, "off-before") == 0);
    bool offAfter = (strcmp(mode, "off-after") == 0);
    if (!offBefore && !offAfter && strcmp(mode, "default") != 0) {
        Die("mode must be default | off-before | off-after");
    }

    printf("[probe] role=%s devid=%u localIp=%s mode=%s\n", role.c_str(), devid, localIp, mode);
    LoadLibra();

    if (offBefore) {
        SwitchOff("before-anything");
    }
    DoPrepareDevice(devid, localIp);
    if (offAfter) {
        SwitchOff("after-RaRdevInit");
    }

    if (role == "server") {
        uint32_t port = (uint32_t)atoi(argv[4]);
        void *sock = MakeSocket(devid, localIp);
        HccpSocketListenInfoT listenInfo{};
        listenInfo.handle = sock;
        listenInfo.port = port;
        auto ret = gRaSocketListenStart(&listenInfo, 1);
        printf("[server] ListenStart(port=%u) ret=%d\n", port, ret);
        if (ret != 0) {
            Die("listen failed");
        }
        if (offAfter) {
            /* also probe the strictest reading: switch off AFTER listen */
            SwitchOff("after-ListenStart");
        }
        ReportSwitch("server-waiting");

        printf("[server] waiting up to 30s for a connect (no white-list entry added)...\n");
        for (int i = 0; i < 300; i++) {
            HccpSocketInfoT info{};
            info.handle = sock;
            info.fd = nullptr;
            info.remoteIp.addr.s_addr = 0;
            info.status = 0;
            uint32_t cnt = 0;
            auto gret = gRaGetSockets(0 /* role=0 server side */, &info, 1, &cnt);
            if (gret == 0 && cnt > 0 && info.fd != nullptr) {
                char peer[INET_ADDRSTRLEN] = {0};
                inet_ntop(AF_INET, &info.remoteIp.addr, peer, sizeof(peer));
                printf("[RESULT] server ACCEPTED fd=%p from %s after %ds --> whitelist NOT enforced (mode=%s)\n",
                       info.fd, peer, i / 10, mode);
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        printf("[RESULT] server got NO connection in 30s --> whitelist (still) enforced (mode=%s)\n", mode);
        return 2;
    }

    if (role == "client") {
        const char *serverIp = argv[4];
        uint32_t port = (uint32_t)atoi(argv[5]);
        void *sock = MakeSocket(devid, localIp);
        HccpSocketConnectInfoT conn{};
        conn.handle = sock;
        if (inet_pton(AF_INET, serverIp, &conn.remoteIp.addr) != 1) {
            Die("invalid server RoCE ip");
        }
        conn.port = port;
        memset(conn.tag, 0, sizeof(conn.tag));
        auto ret = gRaSocketBatchConnect(&conn, 1);
        printf("[client] BatchConnect(%s:%u) ret=%d\n", serverIp, port, ret);
        if (ret != 0) {
            printf("[RESULT] client BatchConnect rejected ret=%d --> whitelist enforced at connect (mode=%s)\n",
                   ret, mode);
            return 2;
        }
        printf("[client] waiting up to 30s for the fd...\n");
        for (int i = 0; i < 300; i++) {
            HccpSocketInfoT info{};
            info.handle = sock;
            info.fd = nullptr;
            info.remoteIp.addr = conn.remoteIp.addr;
            info.status = 0;
            uint32_t cnt = 0;
            auto gret = gRaGetSockets(1 /* role=1 client side */, &info, 1, &cnt);
            if (gret == 0 && cnt > 0 && info.status == 1 && info.fd != nullptr) {
                printf("[RESULT] client CONNECTED fd=%p after %ds --> whitelist NOT enforced (mode=%s)\n", info.fd,
                       i / 10, mode);
                return 0;
            }
            if (gret == 0 && cnt > 0 && info.status == 2) {
                printf("[RESULT] client connect TIMEOUT/rejected (status=2) after %ds --> whitelist enforced (mode=%s)\n",
                       i / 10, mode);
                return 2;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        printf("[RESULT] client no fd in 30s --> whitelist (still) enforced (mode=%s)\n", mode);
        return 2;
    }

    Die("role must be server|client");
}
