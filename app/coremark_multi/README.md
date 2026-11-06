# coremark_multi

Multi-core CoreMark verification for the Orange Pi Zero 3W (Allwinner
A733: 6x Cortex-A55 = cpu0-5 @ max 1794 MHz, 2x Cortex-A76 = cpu6-7 @
max 2002 MHz). Ported from `rk3506-dev/app/coremark_multi` (3x A7) -
the process-per-core design is unchanged, the target and sizing are
new. The purpose is **not** a tailored parallel CoreMark: every run is
a standard, unmodified single-core CoreMark process. Eight independent
processes are pinned to the eight cores with `sched_setaffinity`
(raw syscall, no glibc cpu_set_t), verifying the multi-core features
of the chip:

- **round robin** (`--round-robin`): run one instance per core, one
  core at a time; each core gets its own score
- **parallel** (`--parallel`): run one instance on every core
  simultaneously; each core gets its own score + the combined
  throughput
- `--loops N`: repeat the phase(s) N times (default 2)
- `--cpus 0,1,...`: cores to test (default: all 8)
- `--secs T`: target seconds per run -> iterations sized from the
  measured A76 figure (`EST_CM_PER_S`, 15000 in code; actual 15328)
- every result is appended to `results.csv` (unix_time, mode, loop,
  cpu, iterations, elapsed_s, coremark_s, crc_valid)

Worker isolation: each CoreMark instance is an independent OS process
(fork + exec of this same binary in `--worker` mode), so every
instance has its own address space and heap - the benchmark core is
shared code compiled with `-Dmain=coremark_entry`.

## Frequency pinning (required for meaningful numbers)

`schedutil` would let the two clusters float (416 MHz - cluster max).
`make run-perf` sets the `performance` governor on all 8 cpus first
(A55 -> 1794 MHz, A76 -> 2002 MHz), runs the full round-robin +
parallel suite, then restores `schedutil` (we ssh in as root). With
uniform iterations every run is CRC-valid (> 10 s on A76: 160000
iterations default = 10.4 s there, 22.4 s on A55).

## Build and run

```
make                 # COREMARK_ITERATIONS=160000 default
make push
make run-perf        # pinned clocks, rr + par, 2 loops, ~5.5 min
make run             # quick schedutil round-robin (1 loop)
```

## Results - Orange Pi Zero 3W (all cores at max cluster clock)

Verified run (160000 iterations/run, 2 loops, all 32 results
CRC-validated, performance governor on both clusters):

| Phase  | cpu0-5 (A55, CM/s)   | cpu6 (A76, CM/s) | cpu7 (A76, CM/s) |
|--------|----------------------|------------------|------------------|
| rr L1  | 7154.04 - 7158.84    | 15333.01         | 15337.42         |
| rr L2  | 7156.60 - 7158.84    | 15335.95         | 15341.84         |
| par L1 | 7155.96 - 7158.52    | 15337.42         | 15319.80         |
| par L2 | 7156.28 - 7159.80    | 15325.67         | 15335.95         |

Parallel wall time: 22.361 s per loop (paced by the six A55 cores);
combined parallel throughput **~73600 CoreMark/s** (sum of the eight
per-core scores: 42940 from the A55s + 30660 from the A76s).

Observations:

- Per-core scores are astonishingly repeatable: every A55 result
  across all phases lands in 7154-7160 (spread ~0.08%), every A76
  result in 15320-15342 (~0.15%) - the process-per-core isolation
  works, no cross-contamination.
- Parallel == round-robin within noise: eight independent processes
  with their own heaps show no measurable L2/DDR bandwidth contention
  on this SoC (unlike the rk3506-mini, which lost 8-11% in parallel).
- A76 = ~2.14x one A55 core; matches the single-core `../coremark/`
  results (15328.02 / 7160.76) within 0.1% - two independent apps
  agree.
- "All cores at max frequency" is what makes the numbers comparable:
  the A55 cluster must sit at 1794 MHz and the A76 cluster at
  2002 MHz; the governor was restored to `schedutil` after the run
  (verified on the board).

Raw data: `results.csv` in `~/coremark_multi/` on the board
(regenerate by running).

## Porting notes (arm64 vs the rk3506 armv7 build)

- `linux/core_portme.h`: `ee_ptr_int` must be pointer-sized on
  arm64 (`uintptr_t`, + `#include <stdint.h>`) - the 32-bit type
  truncated heap pointers in `align_mem` (see `../coremark/README`).
- `-mcpu=cortex-a7 ... -marm` flags replaced by `-march=armv8-a`;
  aarch64 has no ARM/Thumb split.
- Default `--cpus` is all 8 (rk3506 defaulted to its 3 cores);
  `MAX_CPUS` 8 fits this SoC exactly.
