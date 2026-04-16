#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# repro_monitor.sh
#
# Helper script to execute the kswapd "spin-without-reclaim" reproducer
# and monitor relevant Virtual Memory (VM) statistics in real-time.
# It tracks buddyinfo (order-0 vs order-3), meminfo, and pgscan_kswapd
# to explicitly demonstrate the thrashing loop behavior.

REPRO_BIN="./pfrag_repro"
TEST_DURATION=30

if [ ! -x "$REPRO_BIN" ]; then
    echo "[!] Error: Executable $REPRO_BIN not found. Please compile it first:"
    echo "    gcc -O2 -o pfrag_repro pfrag_repro.c -lpthread"
    exit 1
fi

echo "======================================================================="
echo "  Phase 0: Preparing Test Environment"
echo "======================================================================="

# 1. Disable page allocation fault injection to ensure we observe the 
#    native allocator behavior rather than artificial failures.
if [ -d /sys/kernel/debug/fail_page_alloc ]; then
    echo "[*] Disabling fail_page_alloc fault injection..."
    echo 0 > /sys/kernel/debug/fail_page_alloc/probability 2>/dev/null
    echo 0 > /sys/kernel/debug/fail_page_alloc/ignore-gfp-highmem 2>/dev/null
else
    echo "[*] fail_page_alloc not found or not mounted. Skipping."
fi

# Record baseline statistics before introducing memory pressure.
echo "[*] Initial kswapd stats in /proc/vmstat:"
grep -E 'pgscan_kswapd|pgsteal_kswapd' /proc/vmstat

echo -e "\n======================================================================="
echo "  Phase 1: Starting Reproducer & Monitoring for ${TEST_DURATION}s"
echo "======================================================================="

# 2. Launch the synthetic reproducer in the background.
$REPRO_BIN &
REPRO_PID=$!

# Allow the reproducer to complete its initial physical memory fragmentation.
sleep 2 

echo "[*] Monitor started. Watch kswapd CPU usage and buddy availability."
echo "---------------------------------------------------------------------------------------------------"
printf "%-10s | %-30s | %-25s | %-25s\n" "TIME" "BUDDYINFO (Normal Zone)" "MEMINFO" "KSWAPD CPU & VMSTAT"
echo "---------------------------------------------------------------------------------------------------"

# 3. Poll core VM metrics to demonstrate the "watermarks-OK but high-order-unavailable" state.
END_TIME=$(( $(date +%s) + TEST_DURATION ))
while [ $(date +%s) -lt $END_TIME ]; do
    CURRENT_TIME=$(date +%H:%M:%S)
    
    # Extract order-0 and order-3 page counts specifically from the Normal zone.
    BUDDY=$(awk '/Normal/ {print "ord0:" $5 " ord3:" $8}' /proc/buddyinfo)
    
    # Extract MemFree and MemAvailable (converted to MB for readability).
    MEMINFO=$(awk '/MemFree/ {free=$2/1024}; /MemAvailable/ {avail=$2/1024}; END {printf "Free:%dMB Avail:%dMB", free, avail}' /proc/meminfo)
    
    # Monitor kswapd0 CPU utilization to detect the 100% spin lockup.
    KSWAPD_CPU=$(top -b -n 1 | awk '/kswapd0/ {print $9"%"}')
    if [ -z "$KSWAPD_CPU" ]; then
        KSWAPD_CPU="0.0%"
    fi

    # Monitor kswapd scanning activity to prove blind thrashing.
    VMSTAT_KSWAPD=$(awk '/pgscan_kswapd/ {print "scan:" $2}' /proc/vmstat)

    # Format and align the output.
    printf "%-10s | %-30s | %-25s | CPU: %-6s %s\n" "$CURRENT_TIME" "$BUDDY" "$MEMINFO" "$KSWAPD_CPU" "$VMSTAT_KSWAPD"
    
    sleep 1
done

echo "---------------------------------------------------------------------------------------------------"
echo "[*] Test duration reached. Cleaning up..."

# 4. Cleanup: Terminate the reproducer and wait for exit.
kill -9 $REPRO_PID 2>/dev/null
wait $REPRO_PID 2>/dev/null

echo "[*] Cleanup done. Review the logs above for kswapd thrashing behavior."
