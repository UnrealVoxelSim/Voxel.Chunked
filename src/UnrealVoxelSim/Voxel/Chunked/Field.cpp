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
	namespace Detail
	{
		constexpr std::int32_t BlockEdge = 32;
		constexpr std::size_t BlockCellCount =
			static_cast<std::size_t>(BlockEdge) * static_cast<std::size_t>(BlockEdge) * static_cast<std::size_t>(
				BlockEdge);

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
			return static_cast<std::int32_t>(static_cast<std::int64_t>(value) - static_cast<std::int64_t>(block) *
				BlockEdge);
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

		[[nodiscard]] std::uint16_t ReadPacked(const std::vector<std::uint64_t>& words,
		                                       const std::uint8_t bits,
		                                       const std::size_t index) noexcept
		{
			const auto bitPosition = index * bits;
			const auto wordIndex = bitPosition / 64U;
			const auto shift = static_cast<unsigned>(bitPosition % 64U);
			const auto mask = (std::uint64_t{1} << bits) - 1U;
			return static_cast<std::uint16_t>((words[wordIndex] >> shift) & mask);
		}

		void WritePacked(std::vector<std::uint64_t>& words,
		                 const std::uint8_t bits,
		                 const std::size_t index,
		                 const std::uint16_t value) noexcept
		{
			const auto bitPosition = index * bits;
			const auto wordIndex = bitPosition / 64U;
			const auto shift = static_cast<unsigned>(bitPosition % 64U);
			const auto mask = ((std::uint64_t{1} << bits) - 1U) << shift;
			words[wordIndex] = (words[wordIndex] & ~mask) | (static_cast<std::uint64_t>(value) << shift);
		}

		Api::CellValue Block::Get(const std::size_t index) const noexcept
		{
			switch (m_Encoding)
			{
			case Encoding::Uniform:
				return m_Uniform;
			case Encoding::Palette:
				return m_Palette[ReadPacked(m_Packed, m_Bits, index)];
			case Encoding::Raw:
				return m_Raw[index];
			}
			assert(false);
			return {};
		}

		bool Block::Set(const std::size_t index, const Api::CellValue desired)
		{
			const auto current = Get(index);
			if (current == desired)
			{
				return false;
			}

			if (current.IsEmpty() && !desired.IsEmpty())
			{
				++m_NonEmptyCount;
			}
			else if (!current.IsEmpty() && desired.IsEmpty())
			{
				--m_NonEmptyCount;
			}

			switch (m_Encoding)
			{
			case Encoding::Uniform:
				m_Palette = {m_Uniform, desired};
				m_Bits = 1;
				m_Packed.assign(PackedWordCount(m_Bits), 0);
				WritePacked(m_Packed, m_Bits, index, 1);
				m_Encoding = Encoding::Palette;
				return true;

			case Encoding::Palette:
				{
					const auto iterator = std::find(m_Palette.begin(), m_Palette.end(), desired);
					if (iterator != m_Palette.end())
					{
						WritePacked(m_Packed, m_Bits, index,
						            static_cast<std::uint16_t>(std::distance(m_Palette.begin(), iterator)));
						return true;
					}

					if (m_Palette.size() < 256)
					{
						const auto oldBits = m_Bits;
						m_Palette.push_back(desired);
						m_Bits = RequiredBits(m_Palette.size());
						if (m_Bits != oldBits)
						{
							std::vector<std::uint64_t> repacked(PackedWordCount(m_Bits), 0);
							for (std::size_t cellIndex = 0; cellIndex < BlockCellCount; ++cellIndex)
							{
								const auto paletteIndex = ReadPacked(m_Packed, oldBits, cellIndex);
								WritePacked(repacked, m_Bits, cellIndex, paletteIndex);
							}
							m_Packed = std::move(repacked);
						}
						WritePacked(m_Packed, m_Bits, index, static_cast<std::uint16_t>(m_Palette.size() - 1));
						return true;
					}

					m_Raw.resize(BlockCellCount);
					for (std::size_t cellIndex = 0; cellIndex < BlockCellCount; ++cellIndex)
					{
						m_Raw[cellIndex] = m_Palette[ReadPacked(m_Packed, m_Bits, cellIndex)];
					}
					m_Raw[index] = desired;
					m_Palette.clear();
					m_Palette.shrink_to_fit();
					m_Packed.clear();
					m_Packed.shrink_to_fit();
					m_Encoding = Encoding::Raw;
					return true;
				}

			case Encoding::Raw:
				m_Raw[index] = desired;
				return true;
			}

			assert(false);
			return false;
		}

		bool Block::IsEmpty() const noexcept
		{
			return m_NonEmptyCount == 0;
		}
	}

	namespace
	{

		struct IndexedMutation final
		{
			std::size_t OriginalIndex{};
			const Api::CellMutation* Mutation{};
		};

		[[nodiscard]] bool PositionLess(const IndexedMutation& left, const IndexedMutation& right) noexcept
		{
			return left.Mutation->Position < right.Mutation->Position;
		}
	}

	Field::Field(const Api::Region bounds) : m_Bounds(bounds)
	{
		if (!bounds.IsValid())
		{
			throw std::invalid_argument{"Voxel field bounds must form a valid region."};
		}
	}

	Field::~Field() = default;

	void Field::AssertOwnerThread() const noexcept
	{
		assert(std::this_thread::get_id() == m_OwnerThread);
	}

	Api::CellValue Field::ReadUnchecked(const Api::Position position) const noexcept
	{
		const auto blockCoordinate = Detail::ToBlock(position);
		const auto iterator = m_Blocks.find(blockCoordinate);
		if (iterator == m_Blocks.end())
		{
			return {};
		}
		return iterator->second.Get(Detail::ToIndex(position, blockCoordinate));
	}

	Api::Region Field::GetBounds() const noexcept
	{
		AssertOwnerThread();
		return m_Bounds;
	}

	std::expected<Api::CellValue, Api::ReadError> Field::Read(const Api::Position position) const noexcept
	{
		AssertOwnerThread();
		if (!m_Bounds.Contains(position))
		{
			return std::unexpected{Api::ReadError::OutOfBounds};
		}
		return ReadUnchecked(position);
	}

	std::expected<void, Api::ReadError> Field::ReadRegion(const Api::Region region,
	                                                      const std::span<Api::CellValue> output) const noexcept
	{
		AssertOwnerThread();
		if (!region.IsValid())
		{
			return std::unexpected{Api::ReadError::InvalidRegion};
		}
		if (!m_Bounds.Contains(region))
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
					const auto blockCoordinate = Detail::ToBlock(start);
					const auto localX = Detail::LocalAxis(x, blockCoordinate.X);
					const auto remainingInBlock = Detail::BlockEdge - localX;
					const auto remainingInRegion = static_cast<std::int64_t>(region.Max.X) - static_cast<std::int64_t>(
						x);
					const auto runLength =
						static_cast<std::int32_t>(std::min<std::int64_t>(remainingInBlock, remainingInRegion));
					const auto iterator = m_Blocks.find(blockCoordinate);
					if (iterator == m_Blocks.end())
					{
						std::fill_n(output.begin() + static_cast<std::ptrdiff_t>(outputIndex), runLength,
						            Api::CellValue{});
						outputIndex += static_cast<std::size_t>(runLength);
					}
					else
					{
						auto index = Detail::ToIndex(start, blockCoordinate);
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
		AssertOwnerThread();
		std::vector<IndexedMutation> ordered;
		ordered.reserve(mutations.size());
		for (std::size_t index = 0; index < mutations.size(); ++index)
		{
			if (!m_Bounds.Contains(mutations[index].Position))
			{
				return std::unexpected{
					Api::EditFailure{
						Api::EditError::OutOfBounds,
						index,
						ReadUnchecked(mutations[index].Position)
					}
				};
			}
			ordered.push_back({index, &mutations[index]});
		}

		std::sort(ordered.begin(), ordered.end(), PositionLess);
		for (std::size_t index = 1; index < ordered.size(); ++index)
		{
			if (ordered[index - 1].Mutation->Position == ordered[index].Mutation->Position)
			{
				return std::unexpected{
					Api::EditFailure{
						Api::EditError::DuplicatePosition,
						std::max(ordered[index - 1].OriginalIndex, ordered[index].OriginalIndex),
						ReadUnchecked(ordered[index].Mutation->Position)
					}
				};
			}
		}

		for (const auto& indexed : ordered)
		{
			const auto actual = ReadUnchecked(indexed.Mutation->Position);
			if (actual != indexed.Mutation->Expected)
			{
				return std::unexpected{Api::EditFailure{Api::EditError::ValueConflict, indexed.OriginalIndex, actual}};
			}
		}

		std::size_t changedCellCount = 0;
		for (const auto& indexed : ordered)
		{
			const auto blockCoordinate = Detail::ToBlock(indexed.Mutation->Position);
			auto [iterator, inserted] = m_Blocks.try_emplace(blockCoordinate);
			static_cast<void>(inserted);
			const auto localIndex = Detail::ToIndex(indexed.Mutation->Position, blockCoordinate);
			if (localIndex >= Detail::BlockCellCount)
			{
				throw std::logic_error{"Computed local voxel index is out of range."};
			}
			if (iterator->second.Set(localIndex, indexed.Mutation->Desired))
			{
				++changedCellCount;
			}
			if (iterator->second.IsEmpty())
			{
				m_Blocks.erase(iterator);
			}
		}

		return Api::EditResult{changedCellCount};
	}
}
