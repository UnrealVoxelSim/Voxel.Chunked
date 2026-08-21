# UnrealVoxelSim.Voxel.Chunked

Sparse, thread-affine in-memory implementation of `UnrealVoxelSim.Voxel.Api`.

`Field` represents one independently owned logical field. Empty space has no allocated backing storage. Internally the
field partitions coordinates into fixed 32-cubed blocks, but partition coordinates and storage layout never cross the
`Voxel.Api` boundary.

Each allocated block starts uniform, changes to a bit-packed palette when values diverge, and falls back to raw 32-bit
values when its palette exceeds 256 entries. Empty blocks are removed. These encodings are replaceable implementation
details and are not persistence formats.

Point operations hash once per accessed block. Region reads traverse logical X-runs and reuse each block lookup rather
than hashing once per cell. Batched edits validate the entire batch's bounds, duplicate positions, and expected values
before committing in deterministic coordinate order.

The implementation contains no locks. All construction, reads, edits, and destruction must occur on the owning thread.
Its release benchmark target covers localized reads, region reads, and batched edits; benchmark conclusions must record
the scenario, compiler, configuration, and hardware.

The initial measured baseline is recorded in [docs/Benchmarks.md](docs/Benchmarks.md).
Use the checked-in `windows-msvc-release-benchmark` or `linux-clang-release-benchmark` configure and build preset to
enable the benchmark-only vcpkg feature and executable.
