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
