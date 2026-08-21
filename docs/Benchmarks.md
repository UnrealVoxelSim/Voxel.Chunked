# Voxel.Chunked benchmark baseline

Measured on 2026-08-21 with:

- AMD Ryzen 5 7600X, 6 cores and 12 logical processors, reported at 4.7 GHz
- 47.1 GiB physical memory
- Microsoft C++ compiler 19.44.35223 for x64
- C++23, MSVC Release configuration
- Google Benchmark 1.9.5

The benchmark executable was run with a 0.05-second minimum scenario time. Results are a development baseline rather
than a cross-machine performance guarantee.

| Scenario | Median reported CPU time | Throughput |
| --- | ---: | ---: |
| Point reads across a 64×64×32 logical region | 24.4 ns/read | — |
| Read an empty 64×64×16 region into a reusable 256 KiB buffer | 52.3 µs | 4.67 GiB/s |
| Toggle a 1,024-cell batch within one storage block | 62.5 µs | 16.38 million edits/s |

The baseline verifies that logical-region abstraction does not require per-cell virtual dispatch. It does not yet
measure representative terrain memory consumption, mixed-palette regional reads, scattered multi-block mutations, or
large-world lookup behavior. Those scenarios should be added before selecting a permanent block size or declaring a
world-scale performance budget.
