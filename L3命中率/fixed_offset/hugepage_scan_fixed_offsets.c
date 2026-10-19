#define _GNU_SOURCE
/*
 * Seed35 fixed offsets; standalone source (no custom header required).
 * Run from this directory on the original Linux/kunpeng host:
 *   cc -O2 -std=c11 -Wall -Wextra -Werror -pthread hugepage_scan_fixed_offsets.c -o scan_fixed
 *   ./scan_fixed 1M 14 fixed 15 seed35_offsets.log 10 10 32 70 --check-fixed
 *   taskset -c 0-15 ./scan_fixed 1M 14 fixed 15 seed35_offsets.log 10 10 32 70
 * Fixed mode reads offsets directly; no seed selects its layout.
 * CPU binding, hugepage allocation, warmup and access/measurement logic
 * are inherited from the original test. Use the existing parent-directory
 * DDR/L3C monitor with the same collection settings as before.
 */
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
#include <ctype.h>

static int fixed_parse_uints(char *p, uint64_t *values, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        while (isspace((unsigned char)*p)) ++p;
        if (*p < '0' || *p > '9') return -1;
        errno = 0;
        char *end;
        unsigned long long value = strtoull(p, &end, 10);
        if (errno || (*end && !isspace((unsigned char)*end))) return -1;
        values[i] = (uint64_t)value;
        p = end;
    }
    while (isspace((unsigned char)*p)) ++p;
    return *p ? -1 : 0;
}

/* Header: fixed_offsets_v1 BLOCK_BYTES THREADS HUGEPAGES SCAN_MIB
 * Rows:   REGION SORTED_PAGE OFFSET_BYTES (decimal, ascending REGION).
 * Blank lines and full-line # comments are accepted. */
static int load_fixed_offsets(const char *path, size_t block, unsigned threads,
                              size_t pages, size_t scan_mib, size_t regions,
                              uint64_t *out)
{
    const uint64_t page_bytes = UINT64_C(1073741824);
    if (!block || block > page_bytes || !threads || !pages || !regions ||
        regions % threads || regions > SIZE_MAX / block ||
        scan_mib > SIZE_MAX / 1048576 ||
        regions * block != scan_mib * 1048576) {
        fprintf(stderr, "Invalid fixed-offset configuration\n");
        return -1;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) { perror(path); return -1; }
    char line[512];
    size_t line_no = 0, row = 0;
    int header = 0, result = -1;
    while (fgets(line, sizeof(line), fp)) {
        ++line_no;
        if (!strchr(line, '\n') && !feof(fp)) goto bad;
        char *p = line;
        while (isspace((unsigned char)*p)) ++p;
        if (!*p || *p == '#') continue;
        if (!header) {
            const char magic[] = "fixed_offsets_v1";
            if (strncmp(p, magic, sizeof(magic)-1) ||
                !isspace((unsigned char)p[sizeof(magic)-1])) goto bad;
            uint64_t v[4];
            if (fixed_parse_uints(p + sizeof(magic)-1, v, 4) ||
                v[0] != block || v[1] != threads || v[2] != pages ||
                v[3] != scan_mib) goto bad;
            header = 1;
            continue;
        }
        uint64_t v[3];
        if (row >= regions || fixed_parse_uints(p, v, 3) ||
            v[0] != row || v[1] != row % pages ||
            v[2] % block || v[2] > page_bytes - block) goto bad;
        for (size_t j = row % pages; j < row; j += pages)
            if (v[2] < out[j] + block && out[j] < v[2] + block) goto bad;
        out[row++] = v[2];
    }
    if (ferror(fp) || !header || row != regions) goto bad;
    result = 0;
    goto done;
bad:
    fprintf(stderr, "%s:%zu: invalid fixed-offset table (rows=%zu, expected=%zu); "
            "check configuration, order, alignment, bounds and overlap\n",
            path, line_no, row, regions);
done:
    fclose(fp);
    return result;
}

#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT 26
#endif
#ifndef MAP_HUGE_1GB
#define MAP_HUGE_1GB (30 << MAP_HUGE_SHIFT)
#endif

#define HUGE_PAGE_SIZE (1ULL * 1024 * 1024 * 1024)
#define MAX_HUGEPAGES  32U
static size_t g_n_hugepages = 32;
#define N_HUGEPAGES    g_n_hugepages
#define ALLOC_SIZE     (N_HUGEPAGES * HUGE_PAGE_SIZE)
#define MAX_REGIONS    1120U
static size_t g_scan_size, g_n_regions;
static unsigned g_n_threads;
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
static int virtual_to_physical(int fd, size_t base_page_size,
                               const void *address, uint64_t *physical)
{
    uint64_t va = (uint64_t)(uintptr_t)address;
    uint64_t entry = 0;
    off_t pos = (off_t)((va / base_page_size) * sizeof(entry));
    ssize_t n;
    do {
        n = pread(fd, &entry, sizeof(entry), pos);
    } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)sizeof(entry)) {
        if (n < 0)
            perror("pread pagemap failed");
        else
            fprintf(stderr, "Short read from pagemap\n");
        return -1;
    }
    if (!(entry & (UINT64_C(1) << 63)) ||
        (entry & (UINT64_C(1) << 62))) {
        fprintf(stderr, "Address %p is not resident\n", address);
        return -1;
    }
    uint64_t pfn = entry & ((UINT64_C(1) << 55) - 1);
    if (pfn == 0) {
        fprintf(stderr, "PFN is zero for %p; physical address cannot be "
                "verified. Check CAP_SYS_ADMIN/pagemap access.\n", address);
        return -1;
    }
    *physical = pfn * base_page_size + va % base_page_size;
    return 0;
}

/* SplitMix64: deterministic address and access sequences. */
static uint64_t next_random(uint64_t *state)
{
    uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}
static uint64_t bounded_random(uint64_t *state, uint64_t bound)
{
    uint64_t threshold = (UINT64_C(0) - bound) % bound, x;
    do { x = next_random(state); } while (x < threshold);
    return x % bound;
}
static uint64_t g_selected_offsets[MAX_REGIONS];
static uint64_t g_fixed_offsets[MAX_REGIONS];
static const char *const mode_names[] = {
    "random", "weighted_hist", "fixed"
};

typedef struct {
    uint8_t *virtual_base;
    uint64_t physical_base;
    size_t original_index;
} hugepage_info_t;

static hugepage_info_t g_sorted_pages[MAX_HUGEPAGES];

static int compare_hugepages(const void *a, const void *b)
{
    const hugepage_info_t *pa = a;
    const hugepage_info_t *pb = b;
    return (pa->physical_base > pb->physical_base)
         - (pa->physical_base < pb->physical_base);
}

/* 只排序地址记录，不移动数据、不改变 mmap 映射。 */
static int load_and_sort_hugepages(void)
{
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
        if (virtual_to_physical(fd, (size_t)page_size,
                                g_sorted_pages[h].virtual_base,
                                &g_sorted_pages[h].physical_base) != 0) {
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
    qsort(g_sorted_pages, N_HUGEPAGES, sizeof(g_sorted_pages[0]),
          compare_hugepages);
    for (size_t h = 1; h < N_HUGEPAGES; h++) {
        if (g_sorted_pages[h - 1].physical_base ==
            g_sorted_pages[h].physical_base) {
            fprintf(stderr, "Duplicate physical hugepage base\n");
            return -1;
        }
    }
    puts("=== Hugepages sorted by physical base ascending ===");
    for (size_t h = 0; h < N_HUGEPAGES; h++) {
        printf("sorted_page=%zu original_page=%zu huge_va=%p "
               "huge_pa=0x%016" PRIx64 "\n", h,
               g_sorted_pages[h].original_index,
               (void *)g_sorted_pages[h].virtual_base,
               g_sorted_pages[h].physical_base);
    }
    return 0;
}

/* 检查当前物理页上所有较早的块，而不只是前一个块。 */
static int offset_in_use(size_t i, uint64_t off)
{
    for (size_t j = i % N_HUGEPAGES; j < i; j += N_HUGEPAGES)
        if (off < g_selected_offsets[j] + g_granularity &&
            g_selected_offsets[j] < off + g_granularity)
            return 1;
    return 0;
}

/* 最大余数法：四段比例9:17:24:14，余数相同时优先较低区间。 */
static void weighted_counts(size_t n, size_t counts[4])
{
    const size_t weights[4] = {9,17,24,14};
    size_t remainder[4], sum=0;
    for (size_t q=0;q<4;q++) {
        counts[q]=n*weights[q]/64;
        remainder[q]=n*weights[q]%64;
        sum+=counts[q];
    }
    while (sum<n) {
        size_t best=0;
        for (size_t q=1;q<4;q++) if (remainder[q]>remainder[best]) best=q;
        counts[best]++; remainder[best]=0; sum++;
    }
}

static int build_region_table(unsigned mode, uint64_t seed)
{
    if (mode>2 || g_n_regions>MAX_REGIONS) return -1;
    if (mode == 2) {
        size_t counts[4] = {0};
        for (size_t i=0; i<g_n_regions; ++i) {
            uint64_t off = g_fixed_offsets[i];
            g_selected_offsets[i] = off;
            g_region_addr_arr[i] = g_sorted_pages[i%N_HUGEPAGES].virtual_base + off;
            counts[off/(256UL*1024*1024)]++;
        }
        printf("mode=fixed seed=0 quarter_counts=[%zu,%zu,%zu,%zu]\n",
               counts[0], counts[1], counts[2], counts[3]);
        return 0;
    }
    uint64_t state=seed;
    uint8_t quarters[MAX_REGIONS];
    size_t target[4]={0};
    const size_t quarter_size=256UL*1024*1024;
    if (mode==1) {
        weighted_counts(g_n_regions,target);
        size_t i=0;
        for (size_t q=0;q<4;q++)
            for (size_t j=0;j<target[q];j++) quarters[i++]=(uint8_t)q;
        /* 固定总配额后随机分配区间标签，不把同一区间集中给前几个线程。 */
        for (size_t n=g_n_regions;n>1;n--) {
            size_t j=bounded_random(&state,n);
            uint8_t t=quarters[n-1]; quarters[n-1]=quarters[j]; quarters[j]=t;
        }
    }
    for (size_t i=0;i<g_n_regions;i++) {
        size_t h=i%N_HUGEPAGES;
        size_t start=mode==1 ? quarters[i]*quarter_size : 0;
        size_t slots=(mode==1 ? quarter_size : HUGE_PAGE_SIZE)/g_granularity;
        uint64_t off;
        do {
            off=start+bounded_random(&state,slots)*g_granularity;
        } while (offset_in_use(i,off));
        g_selected_offsets[i]=off;
        g_region_addr_arr[i]=g_sorted_pages[h].virtual_base+off;
    }
    size_t counts[4]={0};
    for (size_t i=0;i<g_n_regions;i++) {
        uint64_t off=g_selected_offsets[i];
        if (off%g_granularity || off>HUGE_PAGE_SIZE-g_granularity || offset_in_use(i,off)) return -1;
        counts[off/quarter_size]++;
    }
    if (mode==1) for (size_t q=0;q<4;q++) if (counts[q]!=target[q]) return -1;
    printf("mode=%s seed=%" PRIu64 " quarter_counts=[%zu,%zu,%zu,%zu]\n",
           mode_names[mode],seed,counts[0],counts[1],counts[2],counts[3]);
    return 0;
}

/*

- 在初始化和选块完成后、创建工作线程前打印一次。
- tid/begin/count 的划分与下面创建线程时完全相同。
 */
static int print_region_mapping(unsigned int n_threads,
                          size_t regions_per_thread)

{
    puts("=== Selected region mapping (offsets are bytes) ===");
    for (unsigned int tid = 0; tid < n_threads; tid++) {
        size_t begin = tid * regions_per_thread;
        for (size_t j = 0; j < regions_per_thread; j++) {
            size_t idx = begin + j;
            size_t rank = idx % N_HUGEPAGES;
            const hugepage_info_t *hp = &g_sorted_pages[rank];
            uint64_t off = g_selected_offsets[idx];
            printf("tid=%u region=%zu sorted_page=%zu hugepage=%zu "
                   "huge_va=%p huge_pa=0x%016" PRIx64 " "
                   "offset=0x%08" PRIx64 " offset_bytes=%" PRIu64 " "
                   "region_va=%p region_pa=0x%016" PRIx64 "\n",
                   tid, idx, rank, hp->original_index,
                   (void *)hp->virtual_base, hp->physical_base, off, off,
                   (void *)g_region_addr_arr[idx], hp->physical_base + off);
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

static void *worker(void *arg)
{
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
    for (size_t off = 0; off < g_granularity; off += CACHELINE_SIZE)
        (void)*(volatile const uint8_t *)(region + off);
    blocks++;
}
p->blocks = blocks; /* 每线程结束时才写回，避免每个块都更新共享计数。 */
return NULL;

}

static double monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static int run_stage(unsigned seconds, unsigned mode, uint64_t seed,
                     unsigned round, const char *stage)
{
    pthread_t threads[MAX_THREADS];
    th_arg_t args[MAX_THREADS] = {0};
    ready = 0;
    go = 0;
    atomic_store_explicit(&g_stop, 0, memory_order_relaxed);
    unsigned created = 0;
    int result = 0;
    for (unsigned i = 0; i < g_n_threads; i++) {
        args[i] = (th_arg_t){.tid=i, .cpu=(int)i, .begin=i*(g_n_regions/g_n_threads), .count=g_n_regions/g_n_threads};
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
            fprintf(stderr, "Thread %u bind CPU %d: %s\n",
                    i, args[i].cpu, strerror(args[i].error));
            result = 1;
        }
    }
    if (result) atomic_store_explicit(&g_stop, 1, memory_order_relaxed);
    printf("STAGE_BEGIN round=%u mode=%s seed=%" PRIu64
           " stage=%s seconds=%u unix_time=%lld\n",
           round, mode_names[mode], seed, stage, seconds, (long long)time(NULL));
    fflush(stdout);
    double start = monotonic_seconds();
    go = 1;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_mutex);
    if (!result) {
        struct timespec req = {.tv_sec=seconds, .tv_nsec=0};
        while (nanosleep(&req, &req) != 0 && errno == EINTR) {}
    }
    atomic_store_explicit(&g_stop, 1, memory_order_relaxed);
    uint64_t blocks = 0;
    for (unsigned i=0; i<created; i++) {
        pthread_join(threads[i], NULL);
        blocks += args[i].blocks;
    }
    double elapsed = monotonic_seconds()-start;
    /* covered_GiB 是扫过的块跨度，包含重复扫描；不是实际DDR流量。
     * 每64字节仅执行1字节load，因此load_GiB=covered_GiB/64。
     */
    double covered = (double)blocks * g_granularity / (1024.0*1024*1024);
    printf("STAGE_END round=%u mode=%s seed=%" PRIu64
           " stage=%s status=%s elapsed=%.6f blocks=%" PRIu64
           " covered_GiB=%.6f covered_GiB_s=%.6f load_GiB=%.6f\n",
           round, mode_names[mode], seed, stage, result ? "FAIL" : "OK",
           elapsed, blocks, covered, covered/elapsed, covered/CACHELINE_SIZE);
    fflush(stdout);
    return result;
}

static int parse_number(const char *s, uint64_t min, uint64_t max, uint64_t *out)
{
    if (!s[0]) return -1;
    for (const char *p=s; *p; p++) if (*p<'0' || *p>'9') return -1;
    errno = 0;
    char *end;
    unsigned long long value = strtoull(s, &end, 10);
    if (errno || *end || value < min || value > max) return -1;
    *out = value;
    return 0;
}

/*

- Usage: program block threads mode [rounds] [seed] [seconds] [warmup] [pages] [scan_MiB]
- Examples:
- program 128K 14 random        1 18 90 10 32 70
- program 512K 7  weighted_hist 3 18 90 10 20 140
- 配额为全体块的配额，不要求每个线程或每个大页各自满足比例。
 */
int main(int argc, char **argv)
{
 if (argc<4 || argc>11) {
 fprintf(stderr,"Usage: %s 128K|512K|1M 7|14 random|weighted_hist|fixed "
         "[rounds=20] [start_seed=1 OR fixed_table_path] [seconds=90] [warmup=10] "
         "[hugepages=32:16/20/24/28/32] [scan_MiB=70:70/140] [--check-fixed]\n",argv[0]);
 return 1;
 }
 if (!strcmp(argv[1],"128K") || !strcmp(argv[1],"128KB")) g_granularity=128UL*1024;
 else if (!strcmp(argv[1],"512K") || !strcmp(argv[1],"512KB")) g_granularity=512UL*1024;
 else if (!strcmp(argv[1],"1M") || !strcmp(argv[1],"1MB")) g_granularity=1024UL*1024;
 else {fprintf(stderr,"Block size must be 128K, 512K or 1M\n");return 1;}
 if (!strcmp(argv[2],"7")) g_n_threads=7;
 else if (!strcmp(argv[2],"14")) g_n_threads=14;
 else {fprintf(stderr,"Threads must be 7 or 14\n");return 1;}
 unsigned mode;
 if (!strcmp(argv[3],"random")) mode=0;
 else if (!strcmp(argv[3],"weighted_hist")) mode=1;
 else if (!strcmp(argv[3],"fixed")) mode=2;
 else {fprintf(stderr,"Mode must be random, weighted_hist or fixed\n");return 1;}
 int check_fixed = argc == 11 && !strcmp(argv[10], "--check-fixed");
 if ((argc == 11 && (!check_fixed || mode != 2)) || (mode == 2 && argc < 6)) {
     fprintf(stderr,"Fixed mode requires a table path; --check-fixed is fixed-only\n");
     return 1;
 }
 uint64_t rounds=20,start_seed=1,seconds=RUN_SECONDS,warmup=10,hugepages=32,scan_mib=70;
 if ((argc>4 && parse_number(argv[4],1,100000,&rounds)) ||
 (mode!=2 && argc>5 && parse_number(argv[5],0,UINT64_MAX,&start_seed)) ||
 (argc>6 && parse_number(argv[6],1,86400,&seconds)) ||
 (argc>7 && parse_number(argv[7],0,86400,&warmup)) ||
 (argc>8 && parse_number(argv[8],16,32,&hugepages)) ||
 (argc>9 && parse_number(argv[9],70,140,&scan_mib)) ||
 hugepages%4 || (scan_mib!=70 && scan_mib!=140) ||
 (mode!=2 && start_seed>UINT64_MAX-(rounds-1))) {
 fprintf(stderr,"Invalid numeric argument, pages, scan size, or seed overflow\n");return 1;
 }
 g_n_hugepages=(size_t)hugepages;
 g_scan_size=(size_t)scan_mib*1024*1024;
 g_n_regions=g_scan_size/g_granularity;
 if (g_n_regions>MAX_REGIONS || g_scan_size%g_granularity ||
 g_n_regions%g_n_threads || g_n_regions<N_HUGEPAGES) return 1;
 if (mode == 2) {
     start_seed = 0; /* Compatibility log field; never selects fixed offsets. */
     if (load_fixed_offsets(argv[5], g_granularity, g_n_threads, N_HUGEPAGES,
                            (size_t)scan_mib, g_n_regions, g_fixed_offsets)) return 1;
     printf("fixed_table=%s offsets=%zu seed_field=unused\n", argv[5], g_n_regions);
     if (check_fixed) { puts("FIXED_TABLE_OK (no allocation or worker threads)"); return 0; }
 }
 setvbuf(stdout,NULL,_IOLBF,0);
 printf("%zu x 1GiB; scan=%" PRIu64 "MiB; threads=%u; block=%zuKiB; stride=64B\n",
    N_HUGEPAGES,scan_mib,g_n_threads,g_granularity/1024);
 printf("regions=%zu regions/thread=%zu bytes/thread=%zu; CPU=0..%u\n",
    g_n_regions,g_n_regions/g_n_threads,g_scan_size/g_n_threads,g_n_threads-1);
 printf("rounds=%" PRIu64 " start_seed=%" PRIu64 " seconds=%" PRIu64 " warmup=%" PRIu64
    "; fixed per-thread access seeds\n",rounds,start_seed,seconds,warmup);
 printf("Blocks/page: %zu or %zu; first %zu pages get one extra block\n",
    g_n_regions/N_HUGEPAGES,(g_n_regions+N_HUGEPAGES-1)/N_HUGEPAGES,g_n_regions%N_HUGEPAGES);
 g_base=mmap(NULL,ALLOC_SIZE,PROT_READ|PROT_WRITE,
         MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB|MAP_HUGE_1GB,-1,0);
 if (g_base==MAP_FAILED) {perror("mmap 1GiB hugepages");return 1;}
 printf("memset full %zuGiB ...\n",N_HUGEPAGES);
 memset(g_base,0,ALLOC_SIZE);
 puts("memset done");
 g_region_addr_arr=malloc(g_n_regions*sizeof(*g_region_addr_arr));
 if (!g_region_addr_arr) {perror("malloc table");munmap(g_base,ALLOC_SIZE);return 1;}
 int result=load_and_sort_hugepages()!=0;
 for (unsigned r=0;r<rounds && !result;r++) {
 uint64_t seed=mode==2 ? 0 : start_seed+r;
 printf("\nTRIAL_BEGIN round=%u mode=%s seed=%" PRIu64 "\n",r+1,mode_names[mode],seed);
 if (build_region_table(mode,seed)) {fprintf(stderr,"Invalid region table\n");result=1;break;}
 print_region_mapping(g_n_threads,g_n_regions/g_n_threads);
 if (warmup) result=run_stage((unsigned)warmup,mode,seed,r+1,"warmup");
 if (!result) result=run_stage((unsigned)seconds,mode,seed,r+1,"measure");
 printf("TRIAL_END round=%u mode=%s seed=%" PRIu64 "\n",r+1,mode_names[mode],seed);
 }
 free(g_region_addr_arr);
 munmap(g_base,ALLOC_SIZE);
 return result;
}