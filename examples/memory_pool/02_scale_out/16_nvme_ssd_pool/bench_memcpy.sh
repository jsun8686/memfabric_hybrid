#!/usr/bin/env bash
# bench_memcpy.sh -- CPU memcpy bandwidth matrix (per granularity x per thread count).
#
# Motivation: the NVMe-oF tgt (malloc bdev) pays a CPU memcpy per IO on its reactor
# cores; the M=1M experiment degraded 16M read 7.4 -> 4.35 GB/s, consistent with a
# "CPU copy and NIC transfer serialize per command" model whose key parameter is
# the single-core memcpy bandwidth. This tool measures it directly, plus the
# multi-core aggregate (DDR4 node ceiling) and the cross-NUMA penalty.
#
# Self-contained: embeds the C source, compiles with gcc -O2, runs, prints a table.
#
# Usage: bash bench_memcpy.sh [--numa N] [--cross A B] [--threads "1 2 4 8"]
#                             [--sizes "4096 65536 131072 1048576 16777216"] [--iters 32]
#   --numa N    pin all worker threads to node N's cpus (e.g. 6 = NIC/tgt side)
#   --cross A B src buffer first-touched on node A, dst + workers on node B
#               (quantifies the cost of backing memory on the wrong socket)
#   granularity semantics: <=128K rounds measure cache-tier bandwidth on small
#   working sets, >=1M measures real DDR4 bandwidth (region is >= 64MB >> L3).
#
# Output GB/s uses the 1e9 convention (same as example 16's bandwidth matrix).

# re-exec under bash when invoked via sh (dash lacks pipefail)
[ -z "${BASH_VERSION:-}" ] && exec bash "$0" "$@"
set -eu

NUMA=""
CROSS_A=""
CROSS_B=""
THREADS="1 2 4 8"
SIZES="4096 65536 131072 1048576 16777216"
ITERS=32
while [ $# -gt 0 ]; do
    case "$1" in
        --numa) [ $# -ge 2 ] || { echo "--numa needs a value" >&2; exit 1; }; NUMA="$2"; shift ;;
        --numa=*) NUMA="${1#*=}" ;;
        --cross) [ $# -ge 3 ] || { echo "--cross needs two node ids: --cross A B" >&2; exit 1; }; CROSS_A="$2"; CROSS_B="$3"; shift 2 ;;
        --threads) [ $# -ge 2 ] || { echo "--threads needs a value" >&2; exit 1; }; THREADS="$2"; shift ;;
        --threads=*) THREADS="${1#*=}" ;;
        --sizes) [ $# -ge 2 ] || { echo "--sizes needs a value" >&2; exit 1; }; SIZES="$2"; shift ;;
        --sizes=*) SIZES="${1#*=}" ;;
        --iters) [ $# -ge 2 ] || { echo "--iters needs a value" >&2; exit 1; }; ITERS="$2"; shift ;;
        --iters=*) ITERS="${1#*=}" ;;
        *) echo "unknown argument '$1'" >&2; exit 1 ;;
    esac
    shift
done
case "${ITERS}" in ''|*[!0-9]*|0) echo "invalid --iters '${ITERS}'" >&2; exit 1 ;; esac
case "${NUMA}" in ''|*[!0-9]*) [ -n "${NUMA}" ] && { echo "invalid --numa '${NUMA}'" >&2; exit 1; } ;; esac
for n in "${CROSS_A}" "${CROSS_B}"; do
    case "$n" in ''|*[!0-9]*) [ -n "$n" ] && { echo "invalid --cross node '$n'" >&2; exit 1; } ;; esac
done

BIN="$(dirname "$0")/.bench_memcpy"
SRCC="${BIN}.c"

cat > "${SRCC}" <<'EOF'
/* memcpy bandwidth micro-bench: N threads, each copies its own gran-aligned
 * stripe of a >=64MB region (>> L3), 2MB-aligned buffers, warmup round +
 * ROUNDS timed rounds behind pthread barriers, median reported as GB/s (1e9).
 * argv: gran threads iters [bind_cpulist] [src_touch_cpulist]
 *   bind_cpulist    e.g. "144-167" -- workers round-robin onto these cpus
 *   src_touch_cpulist -- when set, src is first-touched on its first cpu
 *   (cross-NUMA mode: src pages on node A, workers/dst on node B) */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sched.h>

#define ROUNDS 5
#define BASE_REGION (64ULL << 20)

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int parse_cpulist(const char *s, int *cpus, int max)
{
    int n = 0;
    while (*s && n < max) {
        int a = 0, b = 0;
        if (sscanf(s, "%d-%d", &a, &b) == 2) {
            for (int i = a; i <= b && n < max; i++) {
                cpus[n++] = i;
            }
        } else if (sscanf(s, "%d", &a) == 1) {
            b = a;
            if (n < max) {
                cpus[n++] = a;
            }
        }
        while (*s && *s != ',') {
            s++;
        }
        if (*s == ',') {
            s++;
        }
    }
    return n;
}

static void pin_to(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    (void)sched_setaffinity(0, sizeof(set), &set);
}

static double g_round[ROUNDS];

struct worker {
    int tid;
    int nthreads;
    int nbind;
    const int *bind;
    size_t gran;
    uint64_t iters;
    size_t nchunk;
    size_t stripe;
    char *src_base;
    char *dst_base;
    pthread_barrier_t *bar;
};

static void *worker_fn(void *arg)
{
    struct worker *w = arg;
    if (w->nbind > 0) {
        pin_to(w->bind[w->tid % w->nbind]);
    }
    char *s = w->src_base + (size_t)w->tid * w->stripe;
    char *d = w->dst_base + (size_t)w->tid * w->stripe;
    /* first-touch own stripe (after pinning: pages land on the right node) */
    for (size_t i = 0; i < w->stripe; i += 4096) {
        s[i] = (char)i;
        d[i] = (char)i;
    }
    for (int r = -1; r < ROUNDS; ++r) {
        pthread_barrier_wait(w->bar);
        double t0 = now_sec();
        for (uint64_t it = 0; it < w->iters; ++it) {
            for (size_t c = 0; c < w->nchunk; ++c) {
                __asm__ __volatile__("" ::: "memory");
                memcpy(d + c * w->gran, s + c * w->gran, w->gran);
            }
            __asm__ __volatile__("" ::: "memory");
        }
        pthread_barrier_wait(w->bar);
        double t1 = now_sec();
        if (r >= 0 && w->tid == 0) {
            g_round[r] = t1 - t0; /* barrier guarantees all threads are done */
        }
    }
    return NULL;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s gran threads iters [bind_cpulist] [src_touch_cpulist]\n", argv[0]);
        return 1;
    }
    size_t gran = strtoull(argv[1], NULL, 0);
    int nth = atoi(argv[2]);
    uint64_t iters = strtoull(argv[3], NULL, 0);
    if (gran == 0 || nth <= 0 || nth > 256 || iters == 0) {
        fprintf(stderr, "bad args\n");
        return 1;
    }
    static int bind[1024];
    int nbind = 0, srccpu = -1;
    if (argc > 4 && argv[4][0] != '\0') {
        nbind = parse_cpulist(argv[4], bind, 1024);
    }
    if (argc > 5 && argv[5][0] != '\0') {
        int t[4];
        if (parse_cpulist(argv[5], t, 4) > 0) {
            srccpu = t[0];
        }
    }

    /* stripe = own chunks per thread; region grows when gran*threads > 64MB */
    size_t nchunk = BASE_REGION / (size_t)nth / gran;
    if (nchunk == 0) {
        nchunk = 1;
    }
    size_t stripe = nchunk * gran;
    size_t region = stripe * (size_t)nth;

    char *src = NULL, *dst = NULL;
    if (posix_memalign((void **)&src, 2ULL << 20, region) != 0 || src == NULL) {
        fprintf(stderr, "src alloc failed\n");
        return 1;
    }
    if (posix_memalign((void **)&dst, 2ULL << 20, region) != 0 || dst == NULL) {
        fprintf(stderr, "dst alloc failed\n");
        return 1;
    }

    /* cross mode: settle src pages on the SOURCE node before workers pin away */
    if (srccpu >= 0) {
        pin_to(srccpu);
        for (size_t i = 0; i < region; i += 4096) {
            src[i] = (char)i;
        }
    }

    pthread_barrier_t bar;
    pthread_barrier_init(&bar, NULL, (unsigned)nth);
    pthread_t th[256];
    static struct worker w[256];
    for (int i = 0; i < nth; i++) {
        w[i].tid = i;
        w[i].nthreads = nth;
        w[i].nbind = nbind;
        w[i].bind = bind;
        w[i].gran = gran;
        w[i].iters = iters;
        w[i].nchunk = nchunk;
        w[i].stripe = stripe;
        w[i].src_base = src;
        w[i].dst_base = dst;
        w[i].bar = &bar;
        if (pthread_create(&th[i], NULL, worker_fn, &w[i]) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            return 1;
        }
    }
    for (int i = 0; i < nth; i++) {
        pthread_join(th[i], NULL);
    }

    qsort(g_round, ROUNDS, sizeof(double), cmp_double);
    double med = g_round[ROUNDS / 2];
    double bytes = (double)iters * (double)nchunk * (double)gran * (double)nth;
    if (med <= 0) {
        printf("0.00");
    } else {
        printf("%.2f", bytes / med / 1e9);
    }
    free(src);
    free(dst);
    return 0;
}
EOF

gcc -O2 -pthread -o "${BIN}" "${SRCC}" || { echo "gcc compile failed (gcc installed?)" >&2; exit 1; }
rm -f "${SRCC}"

# cpulist of a NUMA node, comma-separated ("144,145,...")
cpulist() {
    python3 - "$1" <<'PY'
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
print(",".join(str(c) for c in cpus))
PY
}

BIND=""
SRCTOUCH=""
if [ -n "${CROSS_A}" ] && [ -n "${CROSS_B}" ]; then
    SRCTOUCH=$(cpulist "${CROSS_A}")
    BIND=$(cpulist "${CROSS_B}")
    echo "mode: cross-NUMA copy (src first-touched on node ${CROSS_A}, workers+dst on node ${CROSS_B})"
elif [ -n "${NUMA}" ]; then
    BIND=$(cpulist "${NUMA}")
    echo "mode: local copy, workers pinned to node ${NUMA} (${BIND%%,*}...)"
else
    echo "mode: local copy, no pinning (workers float)"
fi

echo "region per buffer: >=64MB (2MB-aligned), ${ITERS} iterations/round, median of 5 rounds"
echo
printf "%-10s" "gran\\thr"
for t in ${THREADS}; do printf "%9s" "t=${t}"; done
echo
SINGLE_1M=""
for sz in ${SIZES}; do
    printf "%-10s" "${sz}"
    for t in ${THREADS}; do
        v=$("${BIN}" "${sz}" "${t}" "${ITERS}" "${BIND}" "${SRCTOUCH}")
        printf "%9s" "${v}"
        if [ "${t}" = "1" ] && [ "${sz}" -ge 1048576 ] && [ -z "${SINGLE_1M}" ]; then
            SINGLE_1M="${v}"
        fi
    done
    echo
done
echo
echo "legend: rows = memcpy granularity (bytes), columns = worker threads; GB/s = 1e9"
if [ -n "${SINGLE_1M}" ]; then
    echo "model: single-thread DDR4 copy = ${SINGLE_1M} GB/s; the observed tgt degradation"
    echo "       (16M read 7.4 -> 4.35 GB/s at MDTS 1M) matches CPU-copy/NIC-transfer"
    echo "       serialization: B = 1/(1/${SINGLE_1M} + 1/B_nic). E.g. B_nic=20 ->"
    python3 - "${SINGLE_1M}" <<'PY'
import sys
bcpu = float(sys.argv[1])
for bnic in (15.0, 20.0, 25.0):
    print("       B_nic=%.0f -> %.2f GB/s" % (bnic, 1.0 / (1.0 / bcpu + 1.0 / bnic)))
PY
fi
