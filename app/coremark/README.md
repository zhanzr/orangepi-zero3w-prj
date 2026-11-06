# coremark

EEMBC CoreMark 1.0 benchmark, ported from `opz-dev/app/coremark`
(the Orange Pi Zero / H2+ repo; sources ultimately from
`rk3506-dev/app/coremark`). Upstream CoreMark sources + the `linux`
port (`linux/core_portme.*`, POSIX timers, heap memory) are used
unchanged except the two port-level changes an arm64 build needs
(see Porting notes). See `LICENSE.md` (Apache 2.0).

Single binary: aarch64 (`-march=armv8-a`), `-O3 -funroll-loops -flto`.
Runs use `PERFORMANCE_RUN=1`, `SEED_METHOD=SEED_VOLATILE`,
`MEM_METHOD=MEM_MALLOC` (heap), compile-time pinned `ITERATIONS`
(default 200000 - a valid score needs > 10 s per run; the A76 finishes
60000 iterations in ~4 s, far too short).

## Board cores

Allwinner A733, two clusters (verified from `/proc/cpuinfo` CPU part
IDs + cpufreq tables):

| cpus | core | max clock |
|---|---|---|
| cpu0-5 | Cortex-A55 (part 0xd05) | 1794 MHz (9 OPPs 416-1794) |
| cpu6-7 | Cortex-A76 (part 0xd0b) | 2002 MHz (12 OPPs 416-2002) |

`schedutil` is the default governor; `make run-perf` pins
`performance` on all 8 cpus for the run and restores `schedutil`
afterwards (we ssh in as root, no sudo).

## Build and run

```
make                 # or: make ITERATIONS=200000
make push run        # rsync binary to opz3w:~/coremark, run on cpu6 then cpu0
make run-perf        # same but with the performance cpufreq governor
make size            # text/data/bss sizes
```

Runs are pinned with `taskset -c 6` (A76) / `-c 0` (A55).

## Results - Orange Pi Zero 3W (1x A76 @ 2.002 GHz, 1x A55 @ 1.794 GHz)

Validated runs (CRC `0x4983` correct), 200000 iterations each,
GCC 15.2.0 (Ubuntu aarch64-linux-gnu), Armbian 26.11.0-trunk.123,
vendor kernel 6.6.98-sun60iw2, performance governor:

| Core | CoreMark/s | CoreMark/MHz | time |
|---|---|---|---|
| A76 (cpu6, 2002 MHz) | 15328.019620 | 7.66 | 13.05 s |
| A55 (cpu0, 1794 MHz) | 7160.759040 | 3.99 | 27.93 s |

Observations:

- Both runs CRC-validated, > 10 s, performance governor.
- The A76 pair is ~2.1x a single A55 core here - the big cluster is
  where throughput lives (see `../coremark_multi/` for all-core runs).
- Per-clock efficiency: A76 7.66 CM/MHz vs A55 3.99 CM/MHz; the old
  H2+ (Cortex-A7 @ 1.296 GHz) managed 2.86-2.93 CM/MHz (ARM/Thumb).

## Porting notes (arm64 changes vs the armv7 build)

- `linux/core_portme.h`: `typedef ee_u32 ee_ptr_int` ->
  `typedef uintptr_t ee_ptr_int` (+ `#include <stdint.h>`). The
  32-bit type truncated 64-bit heap pointers in the `align_mem` macro
  (core_matrix init) - the portme header itself warns ee_ptr_int must
  hold pointers. With the fix the build is warning-clean except
  upstream's cosmetic `%lu`-with-ee_u32 printfs.
- No ARM/Thumb split on aarch64 (single ISA), no `-mcpu=cortex-a7`
  flags; `-march=armv8-a` runs on both cluster types.
