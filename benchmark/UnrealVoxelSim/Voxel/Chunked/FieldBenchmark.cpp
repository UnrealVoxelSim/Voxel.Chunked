#include "UnrealVoxelSim/Voxel/Chunked/Field.h"

#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>
#include <vector>

namespace UnrealVoxelSim::Voxel::Chunked
{
namespace
{

void PointRead(benchmark::State &state)
{
    Field field{{{0, 0, 0}, {256, 256, 64}}};
    const Api::CellMutation mutation{{31, 31, 31}, {}, Api::CellValue{1}};
    static_cast<void>(field.Apply(std::span{&mutation, 1}));

    std::uint32_t coordinate = 0;
    for (auto _ : state)
    {
        static_cast<void>(_);
        auto value = field.Read({static_cast<std::int32_t>(coordinate & 63U),
                                 static_cast<std::int32_t>((coordinate >> 6U) & 63U),
                                 static_cast<std::int32_t>((coordinate >> 12U) & 31U)});
        benchmark::DoNotOptimize(value);
        ++coordinate;
    }
}

void RegionRead(benchmark::State &state)
{
    Field field{{{0, 0, 0}, {256, 256, 64}}};
    constexpr Api::Region region{{15, 15, 15}, {79, 79, 31}};
    std::vector<Api::CellValue> output(*region.CellCount());

    for (auto _ : state)
    {
        static_cast<void>(_);
        auto result = field.ReadRegion(region, output);
        benchmark::DoNotOptimize(result);
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * output.size() * sizeof(Api::CellValue)));
}

void BatchedEdit(benchmark::State &state)
{
    Field field{{{0, 0, 0}, {64, 64, 64}}};
    std::vector<Api::CellMutation> mutations;
    mutations.reserve(1024);
    for (std::int32_t index = 0; index < 1024; ++index)
    {
        mutations.push_back({{index % 32, (index / 32) % 32, index / (32 * 32)}, {}, Api::CellValue{1}});
    }

    for (auto _ : state)
    {
        static_cast<void>(_);
        auto result = field.Apply(mutations);
        benchmark::DoNotOptimize(result);
        for (auto &mutation : mutations)
        {
            std::swap(mutation.Expected, mutation.Desired);
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(mutations.size()));
}

BENCHMARK(PointRead);
BENCHMARK(RegionRead);
BENCHMARK(BatchedEdit);

} // namespace
} // namespace UnrealVoxelSim::Voxel::Chunked
