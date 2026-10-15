#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>

#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT 26
#endif
#ifndef MAP_HUGE_1GB
#define MAP_HUGE_1GB (30 << MAP_HUGE_SHIFT)
#endif

#define HUGE_PAGE_SIZE (1ULL * 1024 * 1024 * 1024)
#define N_HUGEPAGES    32U
#define ALLOC_SIZE     (N_HUGEPAGES * HUGE_PAGE_SIZE)
#define SCAN_SIZE      (64ULL * 1024 * 1024)
#define MAX_THREADS    32U
#define RUN_SECONDS    90
#define CACHELINE_SIZE 64U
static uint8_t *g_base;
static uint8_t **g_region_addr_arr;
static size_t g_granularity;
static atomic_int g_stop = 0;

typedef struct {
    unsigned int tid;
    int cpu;
    size_t begin;
    size_t count;
    int error;
    uint64_t blocks;
} th_arg_t;
/* pagemap 的索引单位是系统基础页，不是 1GiB 大页。 */

static int virtual_to_physical(int fd, size_t base_page_size, const void *address, uint64_t *physical) {
    uint64_t va = (uint64_t)(uintptr_t)address;
    uint64_t entry = 0;
    off_t pos = (off_t)((va / base_page_size) * sizeof(entry));
    ssize_t n;
    do {
        n = pread(fd, &entry, sizeof(entry), pos);
    }
    while (n < 0 && errno == EINTR);
    if (n != (ssize_t)sizeof(entry)) {
        if (n < 0) perror("pread pagemap failed");
        else fprintf(stderr, "Short read from pagemap\n");
        return -1;
    }
    if (!(entry & (UINT64_C(1) << 63)) || (entry & (UINT64_C(1) << 62))) {
        fprintf(stderr, "Address %p is not resident\n", address);
        return -1;
    }
    uint64_t pfn = entry & ((UINT64_C(1) << 55) - 1);
    if (pfn == 0) {
        fprintf(stderr, "PFN is zero for %p; physical address cannot be " "verified. Check CAP_SYS_ADMIN/pagemap access.\n", address);
        return -1;
    }
    *physical = pfn * base_page_size + va % base_page_size;
    return 0;
}
/* hugepage 表示按物理基址升序排序后的编号；偏移表保持原值。 */

typedef struct {
    size_t hugepage;
    uint64_t offset;
} fixed_region_t;
static const fixed_region_t g_fixed_regions[] = {
    {0, UINT64_C(0x14300000)}, /* tid=0 region=0 */
    {1, UINT64_C(0x26300000)}, /* tid=0 region=1 */
    {2, UINT64_C(0x2fc00000)}, /* tid=0 region=2 */
    {3, UINT64_C(0x3d700000)}, /* tid=0 region=3 */
    {4, UINT64_C(0x16f00000)}, /* tid=1 region=4 */
    {5, UINT64_C(0x2f700000)}, /* tid=1 region=5 */
    {6, UINT64_C(0x33700000)}, /* tid=1 region=6 */
    {7, UINT64_C(0x28500000)}, /* tid=1 region=7 */
    {8, UINT64_C(0x2ad00000)}, /* tid=2 region=8 */
    {9, UINT64_C(0x22e00000)}, /* tid=2 region=9 */
    {10, UINT64_C(0x38500000)}, /* tid=2 region=10 */
    {11, UINT64_C(0x34f00000)}, /* tid=2 region=11 */
    {12, UINT64_C(0x29f00000)}, /* tid=3 region=12 */
    {13, UINT64_C(0x29c00000)}, /* tid=3 region=13 */
    {14, UINT64_C(0x25000000)}, /* tid=3 region=14 */
    {15, UINT64_C(0x38d00000)}, /* tid=3 region=15 */
    {16, UINT64_C(0x34100000)}, /* tid=4 region=16 */
    {17, UINT64_C(0x34800000)}, /* tid=4 region=17 */
    {18, UINT64_C(0x18d00000)}, /* tid=4 region=18 */
    {19, UINT64_C(0x36100000)}, /* tid=4 region=19 */
    {20, UINT64_C(0x3c000000)}, /* tid=5 region=20 */
    {21, UINT64_C(0x19600000)}, /* tid=5 region=21 */
    {22, UINT64_C(0x0f100000)}, /* tid=5 region=22 */
    {23, UINT64_C(0x08300000)}, /* tid=5 region=23 */
    {24, UINT64_C(0x10700000)}, /* tid=6 region=24 */
    {25, UINT64_C(0x1e200000)}, /* tid=6 region=25 */
    {26, UINT64_C(0x1e600000)}, /* tid=6 region=26 */
    {27, UINT64_C(0x1c200000)}, /* tid=6 region=27 */
    {28, UINT64_C(0x21900000)}, /* tid=7 region=28 */
    {29, UINT64_C(0x2eb00000)}, /* tid=7 region=29 */
    {30, UINT64_C(0x13a00000)}, /* tid=7 region=30 */
    {31, UINT64_C(0x34500000)}, /* tid=7 region=31 */
    {0, UINT64_C(0x02000000)}, /* tid=8 region=32 */
    {1, UINT64_C(0x3f500000)}, /* tid=8 region=33 */
    {2, UINT64_C(0x29b00000)}, /* tid=8 region=34 */
    {3, UINT64_C(0x39500000)}, /* tid=8 region=35 */
    {4, UINT64_C(0x24900000)}, /* tid=9 region=36 */
    {5, UINT64_C(0x1ac00000)}, /* tid=9 region=37 */
    {6, UINT64_C(0x1c600000)}, /* tid=9 region=38 */
    {7, UINT64_C(0x15a00000)}, /* tid=9 region=39 */
    {8, UINT64_C(0x14c00000)}, /* tid=10 region=40 */
    {9, UINT64_C(0x1ca00000)}, /* tid=10 region=41 */
    {10, UINT64_C(0x17900000)}, /* tid=10 region=42 */
    {11, UINT64_C(0x1e700000)}, /* tid=10 region=43 */
    {12, UINT64_C(0x2cd00000)}, /* tid=11 region=44 */
    {13, UINT64_C(0x17700000)}, /* tid=11 region=45 */
    {14, UINT64_C(0x02c00000)}, /* tid=11 region=46 */
    {15, UINT64_C(0x07700000)}, /* tid=11 region=47 */
    {16, UINT64_C(0x06500000)}, /* tid=12 region=48 */
    {17, UINT64_C(0x27300000)}, /* tid=12 region=49 */
    {18, UINT64_C(0x28300000)}, /* tid=12 region=50 */
    {19, UINT64_C(0x23400000)}, /* tid=12 region=51 */
    {20, UINT64_C(0x27e00000)}, /* tid=13 region=52 */
    {21, UINT64_C(0x2f400000)}, /* tid=13 region=53 */
    {22, UINT64_C(0x38100000)}, /* tid=13 region=54 */
    {23, UINT64_C(0x2fb00000)}, /* tid=13 region=55 */
    {24, UINT64_C(0x2e100000)}, /* tid=14 region=56 */
    {25, UINT64_C(0x04f00000)}, /* tid=14 region=57 */
    {26, UINT64_C(0x27c00000)}, /* tid=14 region=58 */
    {27, UINT64_C(0x2e000000)}, /* tid=14 region=59 */
    {28, UINT64_C(0x31000000)}, /* tid=15 region=60 */
    {29, UINT64_C(0x05800000)}, /* tid=15 region=61 */
    {30, UINT64_C(0x2d400000)}, /* tid=15 region=62 */
    {31, UINT64_C(0x07300000)}, /* tid=15 region=63 */
};

#define FIXED_REGION_COUNT (sizeof(g_fixed_regions) / sizeof(g_fixed_regions[0]))
/* SplitMix64: deterministic address and access sequences. */

static uint64_t next_random(uint64_t *state) {
    uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static uint64_t bounded_random(uint64_t *state, uint64_t bound) {
    uint64_t threshold = (UINT64_C(0) - bound) % bound, x;
    do {
        x = next_random(state);
    }
    while (x < threshold);
    return x % bound;
}
static uint64_t g_selected_offsets[FIXED_REGION_COUNT];
static const char *const mode_names[] = {"baseline", "random_hist", "random_q2", "shuffle"};

typedef struct {
    uint8_t *virtual_base;
    uint64_t physical_base;
    size_t original_index;
} hugepage_info_t;
static hugepage_info_t g_sorted_pages[N_HUGEPAGES];

static int compare_hugepages(const void *a, const void *b) {
    const hugepage_info_t *pa = a;
    const hugepage_info_t *pb = b;
    return (pa->physical_base > pb->physical_base) - (pa->physical_base < pb->physical_base);
}
/* 只排序地址记录，不移动数据、不改变 mmap 映射。 */

static int load_and_sort_hugepages(void) {
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        fprintf(stderr, "Cannot get system base page size\n");
        return -1;
    }
    int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("open /proc/self/pagemap failed");
        return -1;
    }
    for (size_t h = 0; h < N_HUGEPAGES; h++) {
        g_sorted_pages[h].virtual_base = g_base + h * HUGE_PAGE_SIZE;
        g_sorted_pages[h].original_index = h;
        if (virtual_to_physical(fd, (size_t)page_size, g_sorted_pages[h].virtual_base, &g_sorted_pages[h].physical_base) != 0) {
            close(fd);
            return -1;
        }
        if (g_sorted_pages[h].physical_base % HUGE_PAGE_SIZE != 0) {
            fprintf(stderr, "Hugepage %zu: physical base is not 1GiB aligned\n", h);
            close(fd);
            return -1;
        }
    }
    close(fd);
    qsort(g_sorted_pages, N_HUGEPAGES, sizeof(g_sorted_pages[0]), compare_hugepages);
    for (size_t h = 1; h < N_HUGEPAGES; h++) {
        if (g_sorted_pages[h - 1].physical_base == g_sorted_pages[h].physical_base) {
            fprintf(stderr, "Duplicate physical hugepage base\n");
            return -1;
        }
    }
    puts("=== Hugepages sorted by physical base ascending ===");
    for (size_t h = 0; h < N_HUGEPAGES; h++) {
        printf("sorted_page=%zu original_page=%zu huge_va=%p " "huge_pa=0x%016" PRIx64 "\n", h, g_sorted_pages[h].original_index, (void *)g_sorted_pages[h].virtual_base, g_sorted_pages[h].physical_base);
    }
    return 0;
}
/* random_hist 保留每个 region 原来的256MiB区间，只随机区间内偏移。  * shuffle 将32对基线偏移重新分配给32个物理页排名；线程拥有的页排名不变。  */

static int build_region_table(unsigned mode, uint64_t seed) {
    size_t perm[N_HUGEPAGES];
    for (size_t h = 0; h < N_HUGEPAGES; h++) perm[h] = h;
    uint64_t state = seed;
    if (mode == 3) {
        for (size_t n = N_HUGEPAGES; n > 1; n--) {
            size_t j = bounded_random(&state, n);
            size_t t = perm[n-1];
            perm[n-1] = perm[j];
            perm[j] = t;
        }
    }
    for (size_t i = 0; i < FIXED_REGION_COUNT; i++) {
        size_t h = i % N_HUGEPAGES;
        uint64_t off = g_fixed_regions[i].offset;
        if (mode == 3) off = g_fixed_regions[perm[h] + (i / N_HUGEPAGES) * N_HUGEPAGES].offset;
        if (mode == 1 || mode == 2) {
            uint64_t quarter = mode == 2 ? 2 : off / (256ULL * 1024 * 1024);
            do {
                off = (quarter * 256 + bounded_random(&state, 256)) * g_granularity;
            }
            while (i >= N_HUGEPAGES && off == g_selected_offsets[i-N_HUGEPAGES]);
        }
        g_selected_offsets[i] = off;
        if (off % g_granularity || off > HUGE_PAGE_SIZE-g_granularity) return -1;
        g_region_addr_arr[i] = g_sorted_pages[h].virtual_base + off;
    }
    size_t counts[4] = {0};
    for (size_t i = 0; i < FIXED_REGION_COUNT; i++) {
        counts[g_selected_offsets[i] / (256ULL * 1024 * 1024)]++;
        if (i >= N_HUGEPAGES && g_selected_offsets[i] == g_selected_offsets[i-N_HUGEPAGES]) return -1;
    }
    const size_t normal[] = {9,17,24,14}, concentrated[] = {0,0,64,0};
    const size_t *expected = mode == 2 ? concentrated : normal;
    for (size_t q = 0; q < 4; q++) if (counts[q] != expected[q]) return -1;
    printf("mode=%s seed=%" PRIu64 " quarter_counts=[%zu,%zu,%zu,%zu]\n", mode_names[mode], seed, counts[0], counts[1], counts[2], counts[3]);
    return 0;
}
/*  * 在初始化和选块完成后、创建工作线程前打印一次。  * tid/begin/count 的划分与下面创建线程时完全相同。  */

static int print_region_mapping(unsigned int n_threads, size_t regions_per_thread) {
    puts("=== Selected region mapping (offsets are bytes) ===");
    for (unsigned int tid = 0; tid < n_threads; tid++) {
        size_t begin = tid * regions_per_thread;
        for (size_t j = 0; j < regions_per_thread; j++) {
            size_t idx = begin + j;
            size_t rank = g_fixed_regions[idx].hugepage;
            const hugepage_info_t *hp = &g_sorted_pages[rank];
            uint64_t off = g_selected_offsets[idx];
            printf("tid=%u region=%zu sorted_page=%zu hugepage=%zu " "huge_va=%p huge_pa=0x%016" PRIx64 " " "offset=0x%08" PRIx64 " offset_bytes=%" PRIu64 " " "region_va=%p region_pa=0x%016" PRIx64 "\n", tid, idx, rank, hp->original_index, (void *)hp->virtual_base, hp->physical_base, off, off, (void *)g_region_addr_arr[idx], hp->physical_base + off);
        }
    }
    puts("=== End of mapping ===");
    fflush(stdout);
    return 0;
}
/* 所有线程绑核完成后同步启动；每阶段重置相同的线程访问序列。 */
static pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static unsigned ready;
static int go;

static void *worker(void *arg) {
    th_arg_t *p = arg;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(p->cpu, &cpuset);
    p->error = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    pthread_mutex_lock(&gate_mutex);
    ready++;
    pthread_cond_broadcast(&gate_cond);
    while (!go) pthread_cond_wait(&gate_cond, &gate_mutex);
    pthread_mutex_unlock(&gate_mutex);
    if (p->error) return NULL;
    uint64_t state = UINT64_C(0x123456789abcdef0) + p->tid;
    uint64_t blocks = 0;
    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        size_t idx = p->begin + bounded_random(&state, p->count);
        const uint8_t *region = g_region_addr_arr[idx];
        for (size_t off = 0; off < g_granularity; off += CACHELINE_SIZE) (void)*(volatile const uint8_t *)(region + off);
        blocks++;
    }
    p->blocks = blocks;
    /* 每线程结束时才写回，避免每个块都更新共享计数。 */
    return NULL;
}

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static int run_stage(unsigned seconds, unsigned mode, uint64_t seed, unsigned round, const char *stage) {
    pthread_t threads[16];
    th_arg_t args[16] = {0};
    ready = 0;
    go = 0;
    atomic_store_explicit(&g_stop, 0, memory_order_relaxed);
    unsigned created = 0;
    int result = 0;
    for (unsigned i = 0; i < 16; i++) {
        args[i] = (th_arg_t){.tid=i, .cpu=(int)i, .begin=i*4, .count=4};
        int err = pthread_create(&threads[i], NULL, worker, &args[i]);
        if (err) {
            fprintf(stderr, "pthread_create: %s\n", strerror(err));
            result = 1;
            break;
        }
        created++;
    }
    pthread_mutex_lock(&gate_mutex);
    while (ready < created) pthread_cond_wait(&gate_cond, &gate_mutex);
    for (unsigned i=0; i<created; i++) {
        if (args[i].error) {
            fprintf(stderr, "Thread %u bind CPU %d: %s\n", i, args[i].cpu, strerror(args[i].error));
            result = 1;
        }
    }
    if (result) atomic_store_explicit(&g_stop, 1, memory_order_relaxed);
    printf("STAGE_BEGIN round=%u mode=%s seed=%" PRIu64 " stage=%s seconds=%u unix_time=%lld\n", round, mode_names[mode], seed, stage, seconds, (long long)time(NULL));
    fflush(stdout);
    double start = monotonic_seconds();
    go = 1;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_mutex);
    if (!result) {
        struct timespec req = {.tv_sec=seconds, .tv_nsec=0};
        while (nanosleep(&req, &req) != 0 && errno == EINTR) {
        }
    }
    atomic_store_explicit(&g_stop, 1, memory_order_relaxed);
    uint64_t blocks = 0;
    for (unsigned i=0; i<created; i++) {
        pthread_join(threads[i], NULL);
        blocks += args[i].blocks;
    }
    double elapsed = monotonic_seconds()-start;
    /* covered_GiB 是扫过的块跨度，包含重复扫描；不是实际DDR流量。      * 每64字节仅执行1字节load，因此load_GiB=covered_GiB/64。      */
    double covered = (double)blocks * g_granularity / (1024.0*1024*1024);
    printf("STAGE_END round=%u mode=%s seed=%" PRIu64 " stage=%s status=%s elapsed=%.6f blocks=%" PRIu64 " covered_GiB=%.6f covered_GiB_s=%.6f load_GiB=%.6f\n", round, mode_names[mode], seed, stage, result ? "FAIL" : "OK", elapsed, blocks, covered, covered/elapsed, covered/CACHELINE_SIZE);
    fflush(stdout);
    return result;
}

static int parse_number(const char *s, uint64_t min, uint64_t max, uint64_t *out) {
    if (!s[0]) return -1;
    for (const char *p=s; *p; p++) if (*p<'0' || *p>'9') return -1;
    errno = 0;
    char *end;
    unsigned long long value = strtoull(s, &end, 10);
    if (errno || *end || value < min || value > max) return -1;
    *out = value;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4 || argc > 8 || (strcmp(argv[1], "1M") && strcmp(argv[1], "1MB")) || strcmp(argv[2], "16")) {
        fprintf(stderr, "Usage: %s 1M 16 baseline|random_hist|random_q2|shuffle|all " "[rounds=20] [start_seed=1] [seconds=90] [warmup=10]\n", argv[0]);
        return 1;
    }
    unsigned first=0, end=4;
    if (strcmp(argv[3], "all")) {
        for (;first<4;first++) if (!strcmp(argv[3], mode_names[first])) break;
        if (first==4) {
            fprintf(stderr, "Unknown mode\n");
            return 1;
        }
        end=first+1;
    }
    uint64_t rounds=20, start_seed=1, seconds=RUN_SECONDS, warmup=10;
    if ((argc>4 && parse_number(argv[4],1,100000,&rounds)) || (argc>5 && parse_number(argv[5],0,UINT64_MAX,&start_seed)) || (argc>6 && parse_number(argv[6],1,86400,&seconds)) || (argc>7 && parse_number(argv[7],0,86400,&warmup)) || start_seed > UINT64_MAX-(rounds-1)) {
        fprintf(stderr, "Invalid numeric argument or seed range overflow\n");
        return 1;
    }
    g_granularity = 1024UL*1024;
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("32 x 1GiB; scan=64MiB; 16 threads; 4 x 1MiB/thread; stride=64B\n" "rounds=%" PRIu64 " start_seed=%" PRIu64 " seconds=%" PRIu64 " warmup=%" PRIu64 "; fixed per-thread access seeds\n", rounds,start_seed,seconds,warmup);
    g_base = mmap(NULL, ALLOC_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB|MAP_HUGE_1GB,-1,0);
    if (g_base==MAP_FAILED) {
        perror("mmap 32 x 1GiB hugepages");
        return 1;
    }
    puts("memset full 32GiB ...");
    memset(g_base,0,ALLOC_SIZE);
    puts("memset done");
    g_region_addr_arr = malloc(FIXED_REGION_COUNT*sizeof(*g_region_addr_arr));
    if (!g_region_addr_arr) {
        perror("malloc table");
        munmap(g_base,ALLOC_SIZE);
        return 1;
    }
    int result = load_and_sort_hugepages()!=0;
    for (unsigned r=0; r<rounds && !result; r++) {
        uint64_t seed=start_seed+r;
        for (unsigned mode=first;mode<end && !result;mode++) {
            printf("\nTRIAL_BEGIN round=%u mode=%s seed=%" PRIu64 "\n",r+1,mode_names[mode],seed);
            if (build_region_table(mode,seed)) {
                fprintf(stderr,"Invalid region table\n");
                result=1;
                break;
            }
            print_region_mapping(16,4);
            if (warmup) result=run_stage((unsigned)warmup,mode,seed,r+1,"warmup");
            if (!result) result=run_stage((unsigned)seconds,mode,seed,r+1,"measure");
            printf("TRIAL_END round=%u mode=%s seed=%" PRIu64 "\n",r+1,mode_names[mode],seed);
        }
    }
    free(g_region_addr_arr);
    munmap(g_base,ALLOC_SIZE);
    return result;
}
