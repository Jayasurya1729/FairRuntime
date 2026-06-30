# FairRuntime

A user-space runtime in C++ that combines a CFS-inspired cooperative scheduler, an arena-based memory allocator, an adaptive resource policy manager, and a telemetry dashboard — simulating how an OS balances task fairness and memory pressure across competing workloads.

## Overview

FairRuntime runs several cooperative "tasks" (green threads, implemented with `ucontext`) that each perform randomized memory allocation workloads. A Linux CFS-style scheduler picks which task runs next based on weighted virtual runtime, an arena allocator manages per-task memory with chunked `mmap` backing and bidirectional free-block coalescing, and a resource manager dynamically throttles a task's scheduling weight if it exceeds memory, allocation-rate, or fairness thresholds. A telemetry layer tracks everything and prints a dashboard at the end of the run.

## Architecture

| Component | Files | Responsibility |
|---|---|---|
| Scheduler | `scheduler.h/.cpp` | Cooperative task scheduling via `ucontext`, CFS-style weighted virtual runtime, nice-value weight mapping |
| Allocator | `allocator.h/.cpp` | Arena-based memory allocator with per-task accounting, chunked `mmap`, free-list management, bidirectional coalescing |
| Resource Manager | `resource_manager.h/.cpp` | Computes adaptive scheduling weight adjustments based on live memory, allocation rate, and runtime-share fairness |
| Telemetry | `telemetry.h/.cpp` | Per-task execution/memory stats, Jain's Fairness Index, edge-triggered resource policy action logging, dashboard rendering |
| Policy | `policy.h` | Shared enums (`SchedulingPolicy`, `TaskStatus`) |
| Entry point | `main.cpp` | Configures policy limits, spawns workload tasks, runs the scheduler, prints results |

## Building

```bash
make
./fairruntime
```

(Adjust the binary name above to match your Makefile's actual output target.)

Requires a POSIX environment with `ucontext.h` and `sys/mman.h` support (Linux/WSL; tested on Ubuntu).

## Sample output

```
========================================================
             RESOURCE-AWARE RUNTIME DASHBOARD
========================================================
## SCHEDULER STATISTICS
--------------------------------------------------------
  Scheduling Policy    : CFS
  Scheduler Mode       : Virtual Runtime Scheduling
  Jain's Fairness Index: 0.87

## MEMORY SUBSYSTEM
Task  Total Allocated   Peak Live Memory  Fragmentation
0     986464            726656            0.00%
3     2126896           1610672           50.00%

## RESOURCE MANAGER ACTIONS
  Throttled task 1 (batch-2)
    Reason : Allocation rate exceeded threshold
    Action : Weight reduced (110 -> 82)
```

## Design notes

- **Coalescing is bidirectional but chunk-local.** Freed blocks merge with both physical neighbors when free, but coalescing does not cross separate `mmap` chunk boundaries, since chunks aren't guaranteed to be virtually contiguous. This caps fragmentation reduction at the chunk level rather than the arena level for workloads spanning multiple chunks.
- **Resource policy actions are edge-triggered.** The resource manager logs a throttle/recovery event only on state transitions, not on every scheduling tick a condition holds, to keep the telemetry log signal-dense.
- **Single scheduling policy.** Earlier iterations supported Round Robin and Weighted policies; the project now standardizes on CFS-style weighted virtual runtime as the primary, fully-developed policy.

## Possible extensions

- Pre-reserve a single large virtual region and commit chunks within it to allow cross-chunk coalescing.
- Per-arena locking instead of a single global allocator mutex, to support a multi-threaded (vs. cooperative single-threaded) scheduler.
- Persist `vruntime_ns` accumulation using the resource manager's dynamically adjusted weight, not just the static nice-derived weight.