# kswap

Minimal reproducer setup for observing `kswapd` behavior under network high-order allocation pressure.

## What this repo contains

- `kswapd.sh`  
  Reproduction script.

- `kswapd_spin_repro.c`  
  Reproducer program used to create fragmentation/high-order pressure conditions.  
  Current reproduction environment:
  - VM memory: **10GB**
  - `RESERVE_MB`: **25MB**

- `skb_kswapd_trace.bt`  
  bpftrace script for tracing network TX/RX path high-order allocation failures and fallback to `order-0` allocations.


## Goal

This setup is used to reproduce and inspect scenarios where high-order allocation pressure persists (especially around `order-3`) 
and to correlate allocator fallback behavior with `kswapd` activity.

## Quick notes
- Use `skb_kswapd_trace.bt` while running the reproducer to observe fallback events in real time.
- bpftrace --version bpftrace v0.23.5

## Result
```bash
---------------------------------------------------------------------------------------------------
TIME       | BUDDYINFO (Normal Zone)        | MEMINFO                   | KSWAPD CPU & VMSTAT      
---------------------------------------------------------------------------------------------------
11:08:11   | ord0:11622 ord3:0              | Free:96MB Avail:1309MB    | CPU: 10.0%  scan:83107932
[*] PHASE 3: Triggering Order-3 Pressure (UDP Storm).
11:08:15   | ord0:52079 ord3:0              | Free:273MB Avail:1300MB   | CPU: 90.9%  scan:85328881
11:08:16   | ord0:102895 ord3:0             | Free:477MB Avail:1309MB   | CPU: 60.0%  scan:85873777
11:08:17   | ord0:115459 ord3:5             | Free:517MB Avail:1284MB   | CPU: 54.5%  scan:86584389
11:08:18   | ord0:115164 ord3:0             | Free:509MB Avail:1107MB   | CPU: 36.4%  scan:87083561
---------------------------------------------------------------------------------------------------
                               Trace Statistics                                 
================================================================================
 1. skb_page_frag_refill total calls     : 24785283
 2. High-Order (O3) direct SUCCESS       : 5812315
 3. High-Order FAILED -> Order-0 FALLBACK: 9007441
 4. Complete allocation failures         : 12033431
 5. kswapd wakeups from socket backlog   : 14822
================================================================================
```
