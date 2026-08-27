#include "UnrealVoxelSim/Voxel/Chunked/Field.h"

#include "UnrealVoxelSim/Voxel/Api/EditError.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace UnrealVoxelSim::Voxel::Chunked
{
namespace
{

using Api::CellMutation;
using Api::CellValue;
using Api::EditError;
using Api::Position;
using Api::Region;

TEST(FieldTest, EmptyInBoundsPositionsReadAsEmpty)
{
    const Field field{{{-64, -64, -64}, {64, 64, 64}}};

    ASSERT_TRUE(field.Read({-33, 0, 0}).has_value());
    EXPECT_TRUE(field.Read({-33, 0, 0})->IsEmpty());
    EXPECT_EQ(field.Read({32, 0, 0})->Value(), 0U);
}

TEST(FieldTest, AddsAndRemovesValuesAcrossInternalBoundaries)
{
    Field field{{{-64, -1, -1}, {64, 1, 1}}};
    const std::array positions{
        Position{-33, 0, 0}, Position{-32, 0, 0}, Position{-1, 0, 0},
        Position{0, 0, 0},   Position{31, 0, 0},  Position{32, 0, 0},
    };
    std::vector<CellMutation> additions;
    for (std::size_t index = 0; index < positions.size(); ++index)
    {
        additions.push_back({positions[index], {}, CellValue{static_cast<std::uint32_t>(index + 1)}});
    }

    const auto added = field.Apply(additions);

    ASSERT_TRUE(added.has_value());
    EXPECT_EQ(added->ChangedCellCount, positions.size());
    for (std::size_t index = 0; index < positions.size(); ++index)
    {
        ASSERT_TRUE(field.Read(positions[index]).has_value());
        EXPECT_EQ(field.Read(positions[index])->Value(), index + 1);
    }

    const CellMutation removal{positions[0], CellValue{1}, {}};
    ASSERT_TRUE(field.Apply(std::span{&removal, 1}).has_value());
    EXPECT_TRUE(field.Read(positions[0])->IsEmpty());
}

TEST(FieldTest, ValidationFailureLeavesTheWholeBatchUnchanged)
{
    Field field{{{0, 0, 0}, {4, 1, 1}}};
    const CellMutation occupied{{2, 0, 0}, {}, CellValue{9}};
    ASSERT_TRUE(field.Apply(std::span{&occupied, 1}).has_value());
    const std::array mutations{
        CellMutation{{1, 0, 0}, {}, CellValue{3}},
        CellMutation{{2, 0, 0}, {}, CellValue{4}},
    };

    const auto result = field.Apply(mutations);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().Error, EditError::ValueConflict);
    EXPECT_EQ(result.error().MutationIndex, 1U);
    EXPECT_EQ(result.error().Actual, CellValue{9});
    EXPECT_TRUE(field.Read({1, 0, 0})->IsEmpty());
    EXPECT_EQ(field.Read({2, 0, 0})->Value(), 9U);
}

TEST(FieldTest, RejectsDuplicatePositionsBeforeChangingState)
{
    Field field{{{0, 0, 0}, {4, 1, 1}}};
    const std::array mutations{
        CellMutation{{1, 0, 0}, {}, CellValue{3}},
        CellMutation{{1, 0, 0}, {}, CellValue{4}},
    };

    const auto result = field.Apply(mutations);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().Error, EditError::DuplicatePosition);
    EXPECT_EQ(result.error().MutationIndex, 1U);
    EXPECT_TRUE(field.Read({1, 0, 0})->IsEmpty());
}

TEST(FieldTest, RegionReadsUseXThenYThenZOrder)
{
    Field field{{{-2, 0, 0}, {3, 2, 1}}};
    const std::array mutations{
        CellMutation{{-1, 0, 0}, {}, CellValue{1}}, CellMutation{{0, 0, 0}, {}, CellValue{2}},
        CellMutation{{1, 0, 0}, {}, CellValue{3}},  CellMutation{{-1, 1, 0}, {}, CellValue{4}},
        CellMutation{{0, 1, 0}, {}, CellValue{5}},  CellMutation{{1, 1, 0}, {}, CellValue{6}},
    };
    ASSERT_TRUE(field.Apply(mutations).has_value());
    std::array<CellValue, 6> output;

    const auto result = field.ReadRegion(Region{{-1, 0, 0}, {2, 2, 1}}, output);

    ASSERT_TRUE(result.has_value());
    for (std::size_t index = 0; index < output.size(); ++index)
    {
        EXPECT_EQ(output[index].Value(), index + 1);
    }
}

TEST(FieldTest, PreservesValuesWhenPaletteFallsBackToRawStorage)
{
    Field field{{{0, 0, 0}, {32, 32, 32}}};
    std::vector<CellMutation> mutations;
    mutations.reserve(257);
    for (std::int32_t index = 0; index < 257; ++index)
    {
        mutations.push_back(
            {{index % 32, (index / 32) % 32, index / (32 * 32)}, {}, CellValue{static_cast<std::uint32_t>(index + 1)}});
    }

    ASSERT_TRUE(field.Apply(mutations).has_value());
    for (std::int32_t index = 0; index < 257; ++index)
    {
        const auto value = field.Read({index % 32, (index / 32) % 32, index / (32 * 32)});
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(value->Value(), static_cast<std::uint32_t>(index + 1));
    }
}

TEST(FieldTest, PaletteGrowthPreservesAValueAtNegativeBlockOrigin)
{
    Field field{{{-64, 0, 0}, {1, 1, 1}}};
    const CellMutation first{{-32, 0, 0}, {}, CellValue{2}};
    ASSERT_TRUE(field.Apply(std::span{&first, 1}).has_value());
    ASSERT_EQ(field.Read({-32, 0, 0})->Value(), 2U);
    const CellMutation second{{-1, 0, 0}, {}, CellValue{3}};

    ASSERT_TRUE(field.Apply(std::span{&second, 1}).has_value());

    EXPECT_EQ(field.Read({-32, 0, 0})->Value(), 2U);
    EXPECT_EQ(field.Read({-1, 0, 0})->Value(), 3U);
}

TEST(FieldTest, PaletteGrowthPreservesAValueAtPositiveBlockOrigin)
{
    Field positiveField{{{0, 0, 0}, {64, 1, 1}}};
    const CellMutation positiveFirst{{0, 0, 0}, {}, CellValue{2}};
    ASSERT_TRUE(positiveField.Apply(std::span{&positiveFirst, 1}).has_value());
    const CellMutation positiveSecond{{31, 0, 0}, {}, CellValue{3}};
    ASSERT_TRUE(positiveField.Apply(std::span{&positiveSecond, 1}).has_value());
    EXPECT_EQ(positiveField.Read({0, 0, 0})->Value(), 2U);
    EXPECT_EQ(positiveField.Read({31, 0, 0})->Value(), 3U);
}

TEST(FieldTest, ReportsBoundsAndBufferErrors)
{
    const Field field{{{0, 0, 0}, {2, 2, 2}}};
    std::array<CellValue, 7> output;

    EXPECT_EQ(field.Read({2, 0, 0}).error(), Api::ReadError::OutOfBounds);
    EXPECT_EQ(field.ReadRegion({{0, 0, 0}, {2, 2, 2}}, output).error(), Api::ReadError::OutputSizeMismatch);
    EXPECT_EQ(field.ReadRegion({{0, 0, 0}, {3, 1, 1}}, output).error(), Api::ReadError::OutOfBounds);
}

}
}
