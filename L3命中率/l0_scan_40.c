#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
/*
 * Four USER-SUPPLIED L0 start VAs and one cachesize in MiB per L0 range.
 * No memory allocation, hugepages, physical-base assumptions, sorting, or PID.
 * Each L0 range is [bases[p], bases[p] + cachesize_mib * 1048576).
 * Select ten distinct 1MiB slots per range, giving 40 selected regions.
 * Offsets are multiples of 1MiB relative to each user-supplied L0 base.
 * One SplitMix64 stream is initialized from seed, then used for L0 pages 0..3.
 *
 * Real buffers: call l0_scan_40(bases, cachesize_mib, seed) in their owner process.
 *   const void *bases[4] = {l0_0, l0_1, l0_2, l0_3};
 *   int rc = l0_scan_40(bases, 40, 35);  // each L0 range is 40MiB
 * Define L0_SCAN_40_NO_MAIN to compile/link this file without its standalone main.
 * The buffers must be allocated, initialized/resident, and remain mapped.
 * Merely passing another process's numerical VAs to a new executable does not
 * make those buffers accessible through /proc/self/pagemap.
 *
 * Standalone build:
 *   gcc -O2 -std=c11 -Wall -Wextra l0_scan_40.c -o l0_scan_40
 * Inputs (VAs must ALREADY be mapped in this very process):
 *   ./l0_scan_40 VA0 VA1 VA2 VA3 CACHESIZE_MIB SEED
 * CACHESIZE_MIB is the length of EACH L0 range, not the sum of four ranges.
 * For example, 40 means four 40MiB L0 ranges; 10MiB is selected from each.
 * The program never creates or initializes these four memory ranges.
 *
 * PA is queried for each selected VA and every base page covered by its 1MiB.
 * l0_region_pa is ONLY the PA of the L0 range's first byte. Without hugepages,
 * region_pa MUST NOT be inferred by adding offset to l0_region_pa.
 * physical_contiguous=no means region_pa describes only the first byte of a
 * physically fragmented 1MiB region. No memory contents are read by the query.
 * The observations do not pin pages or prevent later mapping changes.
 * PFN visibility requires suitable pagemap access/CAP_SYS_ADMIN permissions.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>

#define L0_COUNT 4U
#define REGIONS_PER_L0 10U
#define REGION_COUNT (L0_COUNT * REGIONS_PER_L0)
#define REGION_BYTES UINT64_C(1048576)

typedef struct {
    unsigned l0_page;
    uint64_t offset, region_va, region_pa;
    unsigned physical_runs;
} selected_region_t;

/* Original test's SplitMix64 and unbiased bounded sampling. */
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

static int select_regions(const void *const bases[L0_COUNT],
                          const size_t lengths[L0_COUNT], uint64_t seed,
                          selected_region_t regions[REGION_COUNT])
{
    for (unsigned p = 0; p < L0_COUNT; ++p) {
        uintptr_t start = (uintptr_t)bases[p];
        if (!start || lengths[p] / REGION_BYTES < REGIONS_PER_L0 ||
            lengths[p] > UINTPTR_MAX - start) {
            fprintf(stderr, "l0_page=%u: invalid address/range; length must be "
                    "at least 10MiB and VA+length must not overflow\n", p);
            return -1;
        }
        for (unsigned q = 0; q < p; ++q) {
            uintptr_t other = (uintptr_t)bases[q];
            if (start < other + lengths[q] && other < start + lengths[p]) {
                fprintf(stderr, "L0 ranges %u and %u overlap\n", q, p);
                return -1;
            }
        }
    }
    uint64_t state = seed;
    for (unsigned p = 0; p < L0_COUNT; ++p) {
        uint64_t slots = lengths[p] / REGION_BYTES;
        for (unsigned j = 0; j < REGIONS_PER_L0; ++j) {
            uint64_t off;
            int duplicate;
            do {
                off = bounded_random(&state, slots) * REGION_BYTES;
                duplicate = 0;
                for (unsigned k = 0; k < j; ++k)
                    if (regions[p*REGIONS_PER_L0+k].offset == off) duplicate = 1;
            } while (duplicate);
            regions[p*REGIONS_PER_L0+j] = (selected_region_t){
                .l0_page=p, .offset=off,
                .region_va=(uint64_t)(uintptr_t)bases[p]+off
            };
        }
    }
    return 0;
}

/* Same pagemap translation as the original test, with range/overflow checks. */
static int virtual_to_physical(int fd, uint64_t page_bytes, uint64_t va,
                               uint64_t *physical)
{
    uint64_t index = va / page_bytes;
    if (index > (uint64_t)INT64_MAX / sizeof(uint64_t)) {
        fprintf(stderr, "Pagemap offset overflow for VA=0x%016" PRIx64 "\n", va);
        return -1;
    }
    uint64_t entry = 0;
    ssize_t n;
    do {
        n = pread(fd, &entry, sizeof(entry), (off_t)(index * sizeof(entry)));
    } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)sizeof(entry)) {
        fprintf(stderr, "pagemap VA=0x%016" PRIx64 ": %s\n", va,
                n < 0 ? strerror(errno) : "short read");
        return -1;
    }
    if (!(entry & (UINT64_C(1)<<63)) || (entry & (UINT64_C(1)<<62))) {
        fprintf(stderr, "VA=0x%016" PRIx64 " is unmapped, not resident, or swapped; "
                "use initialized buffers in this process\n", va);
        return -1;
    }
    uint64_t pfn = entry & ((UINT64_C(1)<<55)-1);
    uint64_t within_page = va % page_bytes;
    if (!pfn) {
        fprintf(stderr, "PFN=0 for VA=0x%016" PRIx64 "; cannot verify PA. "
                "Check CAP_SYS_ADMIN/pagemap permissions\n", va);
        return -1;
    }
    if (pfn > (UINT64_MAX-within_page)/page_bytes) {
        fprintf(stderr, "Physical address overflow\n");
        return -1;
    }
    *physical = pfn*page_bytes + within_page;
    return 0;
}

static int check_region_mapping(int fd, uint64_t page_bytes, selected_region_t *r)
{
    uint64_t previous_pa=0, previous_bytes=0;
    r->physical_runs = 0;
    for (uint64_t off=0; off<REGION_BYTES;) {
        uint64_t va=r->region_va+off, pa;
        if (virtual_to_physical(fd, page_bytes, va, &pa)) return -1;
        uint64_t bytes=page_bytes-va%page_bytes;
        if (bytes>REGION_BYTES-off) bytes=REGION_BYTES-off;
        if (pa>UINT64_MAX-(bytes-1)) return -1;
        if (!off) r->region_pa=pa;
        if (!off || previous_pa>UINT64_MAX-previous_bytes ||
            previous_pa+previous_bytes!=pa) r->physical_runs++;
        previous_pa=pa;
        previous_bytes=bytes;
        off+=bytes;
    }
    return 0;
}

/* Public entry point. All four VAs must belong to the calling process. */
int l0_scan_40(const void *const bases[L0_COUNT], size_t cachesize_mib,
               uint64_t seed)
{
    if (!bases || cachesize_mib < REGIONS_PER_L0 ||
        cachesize_mib > SIZE_MAX / REGION_BYTES) {
        fprintf(stderr, "Supply four VAs and cachesize >=10MiB per L0 range\n");
        return 1;
    }
    size_t lengths[L0_COUNT];
    for (unsigned p=0; p<L0_COUNT; ++p)
        lengths[p]=cachesize_mib*(size_t)REGION_BYTES;
    selected_region_t regions[REGION_COUNT] = {0};
    if (select_regions(bases, lengths, seed, regions)) return 1;
    long base_page=sysconf(_SC_PAGESIZE);
    if (base_page<=0) { fprintf(stderr, "Cannot get base page size\n"); return 1; }
    uint64_t page_bytes=(uint64_t)base_page;
    int fd=open("/proc/self/pagemap", O_RDONLY|O_CLOEXEC);
    if (fd<0) { perror("open /proc/self/pagemap"); return 1; }
    uint64_t base_pa[L0_COUNT];
    int failed=0;
    for (unsigned p=0; p<L0_COUNT && !failed; ++p)
        failed=virtual_to_physical(fd,page_bytes,(uint64_t)(uintptr_t)bases[p],&base_pa[p])!=0;
    for (unsigned i=0; i<REGION_COUNT && !failed; ++i) {
        failed=check_region_mapping(fd,page_bytes,&regions[i])!=0;
        if (failed) fprintf(stderr, "Mapping failed: region=%u l0_page=%u\n",
                            i,regions[i].l0_page);
    }
    close(fd);
    if (failed) return 1; /* Never emit invented or partial successful records. */
    printf("mode=random seed=%" PRIu64 " cachesize_mib=%zu l0_count=%u regions_per_l0=%u "
           "regions=%u region_bytes=%" PRIu64 " base_page_bytes=%" PRIu64 "\n",
           seed,cachesize_mib,L0_COUNT,REGIONS_PER_L0,REGION_COUNT,REGION_BYTES,page_bytes);
    for (unsigned i=0; i<REGION_COUNT; ++i) {
        const selected_region_t *r=&regions[i];
        unsigned p=r->l0_page;
        printf("region=%u region_in_l0=%u l0_page=%u "
               "l0_region_va=0x%" PRIxPTR " l0_region_pa=0x%016" PRIx64
               " l0_region_bytes=%zu offset=0x%08" PRIx64
               " offset_bytes=%" PRIu64 " region_va=0x%" PRIx64
               " region_pa=0x%016" PRIx64 " region_bytes=%" PRIu64
               " physical_contiguous=%s physical_runs=%u\n",
               i,i%REGIONS_PER_L0,p,(uintptr_t)bases[p],base_pa[p],lengths[p],
               r->offset,r->offset,r->region_va,r->region_pa,REGION_BYTES,
               r->physical_runs==1?"yes":"no",r->physical_runs);
    }
    if (fflush(stdout)==EOF) { perror("stdout"); return 1; }
    return 0;
}

#ifndef L0_SCAN_40_NO_MAIN
static int parse_number(const char *s, uint64_t *value)
{
    if (!s[0] || s[0]<'0' || s[0]>'9') return -1;
    errno=0;
    char *end;
    int base=s[0]=='0' && (s[1]=='x' || s[1]=='X') ? 16 : 10;
    unsigned long long v=strtoull(s,&end,base);
    if (errno || end==s || *end || v>UINT64_MAX) return -1;
    *value=(uint64_t)v;
    return 0;
}

static void usage(const char *name)
{
    fprintf(stderr,"Usage: %s VA0 VA1 VA2 VA3 CACHESIZE_MIB SEED\n"
            "Provide four existing L0 start VAs; no memory is allocated.\n"
            "CACHESIZE_MIB is the length of EACH range, >=10, e.g. 40 for 40MiB.\n"
            "Ranges must not overlap. VAs must exist in the calling process.\n"
            "Embedded entry: l0_scan_40(bases,cachesize_mib,seed).\n",name);
}

int main(int argc, char **argv)
{
    if (argc==2 && !strcmp(argv[1],"--help")) { usage(argv[0]); return 0; }
    if (argc!=7) { usage(argv[0]); return 1; }
    const void *bases[L0_COUNT]={0};
    for (unsigned p=0; p<L0_COUNT; ++p) {
        uint64_t va;
        if (parse_number(argv[1+p],&va) || va>UINTPTR_MAX) {
            fprintf(stderr,"Invalid starting VA for l0_page=%u\n",p);
            return 1;
        }
        bases[p]=(const void *)(uintptr_t)va;
    }
    uint64_t cachesize_mib,seed;
    if (parse_number(argv[5],&cachesize_mib) || cachesize_mib>SIZE_MAX ||
        parse_number(argv[6],&seed)) {
        fprintf(stderr,"Invalid cachesize or seed\n"); return 1;
    }
    return l0_scan_40(bases,(size_t)cachesize_mib,seed);
}
#endif
