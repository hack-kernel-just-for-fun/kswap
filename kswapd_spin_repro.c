/*
 * kswapd_spin_repro.c
 * * A targeted synthetic reproducer for demonstrating the "Spin-without-Reclaim"
 * mechanism disconnect between kswapd and kcompactd in Linux mm/vmscan.c.
 *
 * Authored for LSFMM+BPF Summit discussion.
 * * Strategy:
 * 1. Physical Pinning: Fragment memory using mlock() to create continuous Order-0 
 * free pages while strictly preventing Order-3+ page formation.
 * 2. LRU Saturation: Synthetically generate continuous MADV_FREE pages to keep 
 * the LRU lists populated. This prevents kswapd from hitting the 16-scan 
 * failure limit and going to sleep.
 * 3. High-Order Demand: Trigger high-order (Order-3) allocation requests via 
 * UDP socket backlog.
 * * Expected Result:
 * kswapd enters a 100% CPU spin loop. It successfully reclaims Order-0 pages 
 * (pgsteal increases), but compaction is persistently deferred. The state machine 
 * fails to break the loop, causing severe system thrashing.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/resource.h>
#include <pthread.h>

#ifndef MADV_FREE
#define MADV_FREE 8
#endif

#define PAGE_SIZE           4096
#define ORDER_3_SIZE        (32 * 1024)
#define HUGE_PAGE_SIZE      (2UL * 1024 * 1024)
#define TX_BUF_SIZE         30000 
#define LOOPBACK_ADDR       "127.0.0.1"
#define PORT_STORM          10000
#define PORT_HOLE_BASE      20000
#define NUM_HOLES           500
#define NUM_STORM_THREADS   4

/* Retain a minimal 25MB emergency zone to squeeze the watermarks */
#define RESERVE_MB          25 

int holes[NUM_HOLES];
struct sockaddr_in hole_addrs[NUM_HOLES];

char *arena_mem = NULL;
size_t arena_sz = 0;

static unsigned long long get_free_mem_bytes(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char buf[256];
    unsigned long long free_kb = 0;
    while (fgets(buf, sizeof(buf), f)) {
        if (sscanf(buf, "MemFree: %llu kB", &free_kb) == 1) break;
    }
    fclose(f);
    return free_kb * 1024ULL;
}

static int get_high_order_count(void) {
    FILE *f = fopen("/proc/buddyinfo", "r");
    if (!f) return 0;
    char buf[512];
    int total = 0;
    while (fgets(buf, sizeof(buf), f)) {
        if (!strstr(buf, "Normal")) continue; 
        char *p = strtok(buf, " \t\n");
        int col = 0, val_idx = 0;
        int vals[15] = {0};
        while (p != NULL) {
            if (col >= 4) vals[val_idx++] = atoi(p);
            col++;
            p = strtok(NULL, " \t\n");
        }
        for (int i = 3; i < val_idx; i++) total += vals[i];
    }
    fclose(f);
    return total;
}

/* * Phase 1: Induce strict physical fragmentation.
 * Leaves 25MB reserve, locks the rest, and punches 4KB holes every 32KB.
 */
static void fragment_memory_strictly(void) {
    struct rlimit rl = {RLIM_INFINITY, RLIM_INFINITY};
    setrlimit(RLIMIT_MEMLOCK, &rl);

    unsigned long long free_mem = get_free_mem_bytes();
    arena_sz = free_mem - (RESERVE_MB * 1024ULL * 1024ULL);
    arena_sz &= ~(HUGE_PAGE_SIZE - 1); 
    
    /* Fixed %llu to %zu for size_t */
    printf("[*] PHASE 1: Pinning and fragmenting %zu MB of physical memory...\n", arena_sz / 1024 / 1024);

    arena_mem = mmap(NULL, arena_sz + HUGE_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    arena_mem = (char *)(((unsigned long)arena_mem + HUGE_PAGE_SIZE - 1) & ~(HUGE_PAGE_SIZE - 1));
    
    /* Utilize THP to guarantee physical contiguity before locking */
    madvise(arena_mem, arena_sz, MADV_HUGEPAGE); 
    for (size_t i = 0; i < arena_sz; i += PAGE_SIZE) arena_mem[i] = 0xAA;
    mlock(arena_mem, arena_sz);

    /* Create isolated 4KB holes tagged with MADV_FREE to populate LRU */
    for (size_t i = 0; i < arena_sz; i += ORDER_3_SIZE) {
        munlock(arena_mem + i + ORDER_3_SIZE - PAGE_SIZE, PAGE_SIZE);
        madvise(arena_mem + i + ORDER_3_SIZE - PAGE_SIZE, PAGE_SIZE, MADV_FREE);
    }
    printf("[+] Memory fragmented. Order-3 formation is physically blocked.\n");
}

/* * Helper Thread: The "Sniper".
 * Actively monitors /proc/buddyinfo and consumes any residual Order-3 pages 
 * into socket queues to ensure absolute zero availability for the storm.
 */
void *sniper_thread(void *arg) {
    int tx_fd = socket(AF_INET, SOCK_DGRAM, 0);
    char *buf = malloc(TX_BUF_SIZE);
    memset(buf, 0x42, TX_BUF_SIZE);
    int hole_idx = 0;

    while (1) {
        if (get_high_order_count() > 0) {
            sendto(tx_fd, buf, TX_BUF_SIZE, 0, (struct sockaddr *)&hole_addrs[hole_idx], sizeof(hole_addrs[0]));
            hole_idx = (hole_idx + 1) % NUM_HOLES;
        } else {
            usleep(1000); 
        }
    }
    return NULL;
}

/* * Helper Thread: The "LRU Feeder".
 * Continuously touches and MADV_FREEs the 4KB holes. This synthetic load 
 * guarantees that kswapd's scan will never return 0 pages reclaimed, effectively
 * bypassing the max_reclaim_retries (16) backoff logic.
 */
void *lru_feeder_thread(void *arg) {
    while(1) {
        for (size_t i = 0; i < arena_sz; i += ORDER_3_SIZE) {
            char *hole = arena_mem + i + ORDER_3_SIZE - PAGE_SIZE;
            *hole = 0x42; /* Fault in the physical page */
            madvise(hole, PAGE_SIZE, MADV_FREE); /* Mark as easily reclaimable */
        }
    }
    return NULL;
}

static void setup_infrastructure(void) {
    printf("[*] PHASE 2: Initializing SKB blackholes and background threads...\n");
    for (int i = 0; i < NUM_HOLES; i++) {
        holes[i] = socket(AF_INET, SOCK_DGRAM, 0);
        int rcvbuf = 1024 * 1024 * 2; 
        setsockopt(holes[i], SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof(rcvbuf));
        hole_addrs[i].sin_family = AF_INET;
        hole_addrs[i].sin_port = htons(PORT_HOLE_BASE + i);
        inet_pton(AF_INET, LOOPBACK_ADDR, &hole_addrs[i].sin_addr);
        bind(holes[i], (struct sockaddr *)&hole_addrs[i], sizeof(hole_addrs[i]));
    }

    pthread_t sniper, feeder;
    pthread_create(&sniper, NULL, sniper_thread, NULL);
    pthread_create(&feeder, NULL, lru_feeder_thread, NULL);

    /* Allow threads to stabilize and drain residual O3 pages */
    sleep(3);
    printf("[*] PHASE 3: Triggering UDP Storm. Monitoring kswapd behavior...\n");
}

/* UDP Storm RX */
void *rx_thread(void *arg) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(PORT_STORM) };
    inet_pton(AF_INET, LOOPBACK_ADDR, &addr.sin_addr);
    bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    char *buf = malloc(TX_BUF_SIZE);
    while(1) recv(fd, buf, TX_BUF_SIZE, 0);
    return NULL;
}

/* UDP Storm TX: Generates immense pressure for Order-3 allocations */
void *tx_thread(void *arg) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in dst = { .sin_family = AF_INET, .sin_port = htons(PORT_STORM) };
    inet_pton(AF_INET, LOOPBACK_ADDR, &dst.sin_addr);
    char *buf = malloc(TX_BUF_SIZE);
    memset(buf, 0x42, TX_BUF_SIZE);
    while(1) sendto(fd, buf, TX_BUF_SIZE, 0, (struct sockaddr *)&dst, sizeof(dst));
    return NULL;
}

int main(void) {
    int sys_ret; /* Added to handle warn_unused_result */

    if (geteuid() != 0) { 
        fprintf(stderr, "Error: Root privileges required for mlock() and sysctl adjustments.\n"); 
        return 1; 
    }
    
    printf("=== kswapd Spin-without-Reclaim Synthetic Reproducer ===\n");
    printf("[*] PHASE 0: Purging caches to establish baseline...\n");
    
    /* Properly silenced the warning */
    sys_ret = system("sync; echo 3 > /proc/sys/vm/drop_caches");
    (void)sys_ret; 
    
    sleep(1);

    fragment_memory_strictly();
    setup_infrastructure();
    
    pthread_t rx;
    pthread_create(&rx, NULL, rx_thread, NULL);
    
    /* Fixed undeclared variable */
    pthread_t tx[NUM_STORM_THREADS];
    for(int i = 0; i < NUM_STORM_THREADS; i++) {
        pthread_create(&tx[i], NULL, tx_thread, NULL);
    }
    
    for(int i = 0; i < NUM_STORM_THREADS; i++) {
        pthread_join(tx[i], NULL);
    }
    
    return 0;
}
