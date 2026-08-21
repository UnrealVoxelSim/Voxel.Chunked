#include "UnrealVoxelSim/Voxel/Chunked/Field.h"

#include <algorithm>
#include <cassert>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace UnrealVoxelSim::Voxel::Chunked
{
namespace
{

constexpr std::int32_t BlockEdge = 32;
constexpr std::size_t BlockCellCount =
    static_cast<std::size_t>(BlockEdge) * static_cast<std::size_t>(BlockEdge) * static_cast<std::size_t>(BlockEdge);

struct BlockCoordinate final
{
    std::int32_t X{};
    std::int32_t Y{};
    std::int32_t Z{};

    auto operator<=>(const BlockCoordinate &) const = default;
};

struct BlockCoordinateHash final
{
    [[nodiscard]] std::size_t operator()(const BlockCoordinate coordinate) const noexcept
    {
        auto hash = std::hash<std::int32_t>{}(coordinate.X);
        hash ^= std::hash<std::int32_t>{}(coordinate.Y) + static_cast<std::size_t>(0x9E3779B9U) + (hash << 6U) +
                (hash >> 2U);
        hash ^= std::hash<std::int32_t>{}(coordinate.Z) + static_cast<std::size_t>(0x9E3779B9U) + (hash << 6U) +
                (hash >> 2U);
        return hash;
    }
};

struct BlockCoordinateEqual final
{
    [[nodiscard]] bool operator()(const BlockCoordinate left, const BlockCoordinate right) const noexcept
    {
        return left.X == right.X && left.Y == right.Y && left.Z == right.Z;
    }
};

[[nodiscard]] constexpr std::int32_t BlockAxis(const std::int32_t value) noexcept
{
    if (value >= 0)
    {
        return value / BlockEdge;
    }
    // Widen before biasing so floor division is correct for exact multiples and INT32_MIN.
    return static_cast<std::int32_t>((static_cast<std::int64_t>(value) - (BlockEdge - 1)) / BlockEdge);
}

static_assert(BlockAxis(-33) == -2);
static_assert(BlockAxis(-32) == -1);
static_assert(BlockAxis(-1) == -1);
static_assert(BlockAxis(0) == 0);
static_assert(BlockAxis(32) == 1);

[[nodiscard]] constexpr std::int32_t LocalAxis(const std::int32_t value, const std::int32_t block) noexcept
{
    return static_cast<std::int32_t>(static_cast<std::int64_t>(value) - static_cast<std::int64_t>(block) * BlockEdge);
}

[[nodiscard]] constexpr BlockCoordinate ToBlock(const Api::Position position) noexcept
{
    return {BlockAxis(position.X), BlockAxis(position.Y), BlockAxis(position.Z)};
}

[[nodiscard]] constexpr std::size_t ToIndex(const Api::Position position, const BlockCoordinate block) noexcept
{
    const auto x = static_cast<std::size_t>(LocalAxis(position.X, block.X));
    const auto y = static_cast<std::size_t>(LocalAxis(position.Y, block.Y));
    const auto z = static_cast<std::size_t>(LocalAxis(position.Z, block.Z));
    return x + static_cast<std::size_t>(BlockEdge) * (y + static_cast<std::size_t>(BlockEdge) * z);
}

[[nodiscard]] constexpr std::uint8_t RequiredBits(const std::size_t paletteSize) noexcept
{
    if (paletteSize <= 2)
    {
        return 1;
    }
    if (paletteSize <= 4)
    {
        return 2;
    }
    if (paletteSize <= 16)
    {
        return 4;
    }
    return 8;
}

[[nodiscard]] constexpr std::size_t PackedWordCount(const std::uint8_t bits) noexcept
{
    return (BlockCellCount * bits + 63U) / 64U;
}

[[nodiscard]] std::uint16_t ReadPacked(const std::vector<std::uint64_t> &words, const std::uint8_t bits,
                                       const std::size_t index) noexcept
{
    const auto bitPosition = index * bits;
    const auto wordIndex = bitPosition / 64U;
    const auto shift = static_cast<unsigned>(bitPosition % 64U);
    const auto mask = (std::uint64_t{1} << bits) - 1U;
    return static_cast<std::uint16_t>((words[wordIndex] >> shift) & mask);
}

void WritePacked(std::vector<std::uint64_t> &words, const std::uint8_t bits, const std::size_t index,
                 const std::uint16_t value) noexcept
{
    const auto bitPosition = index * bits;
    const auto wordIndex = bitPosition / 64U;
    const auto shift = static_cast<unsigned>(bitPosition % 64U);
    const auto mask = ((std::uint64_t{1} << bits) - 1U) << shift;
    words[wordIndex] = (words[wordIndex] & ~mask) | (static_cast<std::uint64_t>(value) << shift);
}

class Block final
{
  public:
    [[nodiscard]] Api::CellValue Get(const std::size_t index) const noexcept
    {
        switch (Encoding_)
        {
        case Encoding::Uniform:
            return Uniform_;
        case Encoding::Palette:
            return Palette_[ReadPacked(Packed_, Bits_, index)];
        case Encoding::Raw:
            return Raw_[index];
        }
        assert(false);
        return {};
    }

    [[nodiscard]] bool Set(const std::size_t index, const Api::CellValue desired)
    {
        const auto current = Get(index);
        if (current == desired)
        {
            return false;
        }

        if (current.IsEmpty() && !desired.IsEmpty())
        {
            ++NonEmptyCount_;
        }
        else if (!current.IsEmpty() && desired.IsEmpty())
        {
            --NonEmptyCount_;
        }

        switch (Encoding_)
        {
        case Encoding::Uniform:
            Palette_ = {Uniform_, desired};
            Bits_ = 1;
            Packed_.assign(PackedWordCount(Bits_), 0);
            WritePacked(Packed_, Bits_, index, 1);
            Encoding_ = Encoding::Palette;
            return true;

        case Encoding::Palette: {
            const auto iterator = std::find(Palette_.begin(), Palette_.end(), desired);
            if (iterator != Palette_.end())
            {
                WritePacked(Packed_, Bits_, index,
                            static_cast<std::uint16_t>(std::distance(Palette_.begin(), iterator)));
                return true;
            }

            if (Palette_.size() < 256)
            {
                const auto oldBits = Bits_;
                Palette_.push_back(desired);
                Bits_ = RequiredBits(Palette_.size());
                if (Bits_ != oldBits)
                {
                    std::vector<std::uint64_t> repacked(PackedWordCount(Bits_), 0);
                    for (std::size_t cellIndex = 0; cellIndex < BlockCellCount; ++cellIndex)
                    {
                        const auto paletteIndex = ReadPacked(Packed_, oldBits, cellIndex);
                        WritePacked(repacked, Bits_, cellIndex, paletteIndex);
                    }
                    Packed_ = std::move(repacked);
                }
                WritePacked(Packed_, Bits_, index, static_cast<std::uint16_t>(Palette_.size() - 1));
                return true;
            }

            Raw_.resize(BlockCellCount);
            for (std::size_t cellIndex = 0; cellIndex < BlockCellCount; ++cellIndex)
            {
                Raw_[cellIndex] = Palette_[ReadPacked(Packed_, Bits_, cellIndex)];
            }
            Raw_[index] = desired;
            Palette_.clear();
            Palette_.shrink_to_fit();
            Packed_.clear();
            Packed_.shrink_to_fit();
            Encoding_ = Encoding::Raw;
            return true;
        }

        case Encoding::Raw:
            Raw_[index] = desired;
            return true;
        }

        assert(false);
        return false;
    }

    [[nodiscard]] bool IsEmpty() const noexcept
    {
        return NonEmptyCount_ == 0;
    }

  private:
    enum class Encoding
    {
        Uniform,
        Palette,
        Raw,
    };

    Encoding Encoding_{Encoding::Uniform};
    Api::CellValue Uniform_{};
    std::vector<Api::CellValue> Palette_;
    std::vector<std::uint64_t> Packed_;
    std::vector<Api::CellValue> Raw_;
    std::size_t NonEmptyCount_{};
    std::uint8_t Bits_{};
};

struct IndexedMutation final
{
    std::size_t OriginalIndex{};
    const Api::CellMutation *Mutation{};
};

[[nodiscard]] bool PositionLess(const IndexedMutation &left, const IndexedMutation &right) noexcept
{
    return left.Mutation->Position < right.Mutation->Position;
}

} // namespace

class Field::Impl final
{
  public:
    explicit Impl(const Api::Region bounds) : Bounds(bounds)
    {
    }

    void AssertOwnerThread() const noexcept
    {
        assert(std::this_thread::get_id() == OwnerThread);
    }

    [[nodiscard]] Api::CellValue ReadUnchecked(const Api::Position position) const noexcept
    {
        const auto blockCoordinate = ToBlock(position);
        const auto iterator = Blocks.find(blockCoordinate);
        if (iterator == Blocks.end())
        {
            return {};
        }
        return iterator->second.Get(ToIndex(position, blockCoordinate));
    }

    Api::Region Bounds;
    std::unordered_map<BlockCoordinate, Block, BlockCoordinateHash, BlockCoordinateEqual> Blocks;
    std::thread::id OwnerThread{std::this_thread::get_id()};
};

Field::Field(const Api::Region bounds)
{
    if (!bounds.IsValid())
    {
        throw std::invalid_argument{"Voxel field bounds must form a valid region."};
    }
    Impl_ = std::make_unique<Impl>(bounds);
}

Field::~Field() = default;

Api::Region Field::Bounds() const noexcept
{
    Impl_->AssertOwnerThread();
    return Impl_->Bounds;
}

std::expected<Api::CellValue, Api::ReadError> Field::Read(const Api::Position position) const noexcept
{
    Impl_->AssertOwnerThread();
    if (!Impl_->Bounds.Contains(position))
    {
        return std::unexpected{Api::ReadError::OutOfBounds};
    }
    return Impl_->ReadUnchecked(position);
}

std::expected<void, Api::ReadError> Field::ReadRegion(const Api::Region region,
                                                      const std::span<Api::CellValue> output) const noexcept
{
    Impl_->AssertOwnerThread();
    if (!region.IsValid())
    {
        return std::unexpected{Api::ReadError::InvalidRegion};
    }
    if (!Impl_->Bounds.Contains(region))
    {
        return std::unexpected{Api::ReadError::OutOfBounds};
    }
    const auto cellCount = region.CellCount();
    if (!cellCount)
    {
        return std::unexpected{Api::ReadError::RegionVolumeOverflow};
    }
    if (output.size() != *cellCount)
    {
        return std::unexpected{Api::ReadError::OutputSizeMismatch};
    }

    std::size_t outputIndex = 0;
    for (auto z = region.Min.Z; z < region.Max.Z; ++z)
    {
        for (auto y = region.Min.Y; y < region.Max.Y; ++y)
        {
            auto x = region.Min.X;
            while (x < region.Max.X)
            {
                const Api::Position start{x, y, z};
                const auto blockCoordinate = ToBlock(start);
                const auto localX = LocalAxis(x, blockCoordinate.X);
                const auto remainingInBlock = BlockEdge - localX;
                const auto remainingInRegion = static_cast<std::int64_t>(region.Max.X) - static_cast<std::int64_t>(x);
                const auto runLength =
                    static_cast<std::int32_t>(std::min<std::int64_t>(remainingInBlock, remainingInRegion));
                const auto iterator = Impl_->Blocks.find(blockCoordinate);
                if (iterator == Impl_->Blocks.end())
                {
                    std::fill_n(output.begin() + static_cast<std::ptrdiff_t>(outputIndex), runLength, Api::CellValue{});
                    outputIndex += static_cast<std::size_t>(runLength);
                }
                else
                {
                    auto index = ToIndex(start, blockCoordinate);
                    for (std::int32_t offset = 0; offset < runLength; ++offset)
                    {
                        output[outputIndex++] = iterator->second.Get(index++);
                    }
                }
                x = static_cast<std::int32_t>(static_cast<std::int64_t>(x) + runLength);
            }
        }
    }
    return {};
}

std::expected<Api::EditResult, Api::EditFailure> Field::Apply(const std::span<const Api::CellMutation> mutations)
{
    Impl_->AssertOwnerThread();
    std::vector<IndexedMutation> ordered;
    ordered.reserve(mutations.size());
    for (std::size_t index = 0; index < mutations.size(); ++index)
    {
        if (!Impl_->Bounds.Contains(mutations[index].Position))
        {
            return std::unexpected{
                Api::EditFailure{Api::EditError::OutOfBounds, index, Impl_->ReadUnchecked(mutations[index].Position)}};
        }
        ordered.push_back({index, &mutations[index]});
    }

    std::sort(ordered.begin(), ordered.end(), PositionLess);
    for (std::size_t index = 1; index < ordered.size(); ++index)
    {
        if (ordered[index - 1].Mutation->Position == ordered[index].Mutation->Position)
        {
            return std::unexpected{
                Api::EditFailure{Api::EditError::DuplicatePosition,
                                 std::max(ordered[index - 1].OriginalIndex, ordered[index].OriginalIndex),
                                 Impl_->ReadUnchecked(ordered[index].Mutation->Position)}};
        }
    }

    for (const auto &indexed : ordered)
    {
        const auto actual = Impl_->ReadUnchecked(indexed.Mutation->Position);
        if (actual != indexed.Mutation->Expected)
        {
            return std::unexpected{Api::EditFailure{Api::EditError::ValueConflict, indexed.OriginalIndex, actual}};
        }
    }

    std::size_t changedCellCount = 0;
    for (const auto &indexed : ordered)
    {
        const auto blockCoordinate = ToBlock(indexed.Mutation->Position);
        auto [iterator, inserted] = Impl_->Blocks.try_emplace(blockCoordinate);
        static_cast<void>(inserted);
        const auto localIndex = ToIndex(indexed.Mutation->Position, blockCoordinate);
        if (localIndex >= BlockCellCount)
        {
            throw std::logic_error{"Computed local voxel index is out of range."};
        }
        if (iterator->second.Set(localIndex, indexed.Mutation->Desired))
        {
            ++changedCellCount;
        }
        if (iterator->second.IsEmpty())
        {
            Impl_->Blocks.erase(iterator);
        }
    }

    return Api::EditResult{changedCellCount};
}

} // namespace UnrealVoxelSim::Voxel::Chunked
