#pragma once

#include "UnrealVoxelSim/Voxel/Api/IBounds.h"
#include "UnrealVoxelSim/Voxel/Api/IEditor.h"
#include "UnrealVoxelSim/Voxel/Api/IReader.h"
#include "UnrealVoxelSim/Voxel/Api/IRegionReader.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace UnrealVoxelSim::Voxel::Chunked
{
	namespace Detail
	{
		struct BlockCoordinate final
		{
			std::int32_t X{};
			std::int32_t Y{};
			std::int32_t Z{};

			auto operator<=>(const BlockCoordinate&) const = default;
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

		class Block final
		{
		public:
			[[nodiscard]] Api::CellValue Get(std::size_t index) const noexcept;
			[[nodiscard]] bool Set(std::size_t index, Api::CellValue desired);
			[[nodiscard]] bool IsEmpty() const noexcept;

		private:
			enum class Encoding
			{
				Uniform,
				Palette,
				Raw,
			};

			Encoding m_Encoding{Encoding::Uniform};
			Api::CellValue m_Uniform{};
			std::vector<Api::CellValue> m_Palette;
			std::vector<std::uint64_t> m_Packed;
			std::vector<Api::CellValue> m_Raw;
			std::size_t m_NonEmptyCount{};
			std::uint8_t m_Bits{};
		};
	}

	class Field final : public Api::IBounds, public Api::IReader, public Api::IRegionReader, public Api::IEditor
	{
	public:
		explicit Field(Api::Region bounds);
		~Field() override;

		Field(const Field&) = delete;
		Field& operator=(const Field&) = delete;
		Field(Field&&) = delete;
		Field& operator=(Field&&) = delete;

		[[nodiscard]] Api::Region GetBounds() const noexcept override;
		[[nodiscard]] std::expected<Api::CellValue, Api::ReadError>
		Read(Api::Position position) const noexcept override;
		[[nodiscard]] std::expected<void, Api::ReadError> ReadRegion(
			Api::Region region,
			std::span<Api::CellValue> output) const noexcept override;
		[[nodiscard]] std::expected<Api::EditResult, Api::EditFailure> Apply(
			std::span<const Api::CellMutation> mutations) override;

	private:
		void AssertOwnerThread() const noexcept;
		[[nodiscard]] Api::CellValue ReadUnchecked(Api::Position position) const noexcept;

		Api::Region m_Bounds;
		std::unordered_map<Detail::BlockCoordinate,
		                   Detail::Block,
		                   Detail::BlockCoordinateHash,
		                   Detail::BlockCoordinateEqual>
			m_Blocks;
		std::thread::id m_OwnerThread{std::this_thread::get_id()};
	};
}
