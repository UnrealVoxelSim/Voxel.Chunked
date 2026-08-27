#pragma once

#include "UnrealVoxelSim/Voxel/Api/IBounds.h"
#include "UnrealVoxelSim/Voxel/Api/IEditor.h"
#include "UnrealVoxelSim/Voxel/Api/IReader.h"
#include "UnrealVoxelSim/Voxel/Api/IRegionReader.h"

#include <memory>

namespace UnrealVoxelSim::Voxel::Chunked
{

class Field final : public Api::IBounds, public Api::IReader, public Api::IRegionReader, public Api::IEditor
{
  public:
    explicit Field(Api::Region bounds);
    ~Field() override;

    Field(const Field &) = delete;
    Field &operator=(const Field &) = delete;
    Field(Field &&) = delete;
    Field &operator=(Field &&) = delete;

    [[nodiscard]] Api::Region Bounds() const noexcept override;
    [[nodiscard]] std::expected<Api::CellValue, Api::ReadError> Read(Api::Position position) const noexcept override;
    [[nodiscard]] std::expected<void, Api::ReadError> ReadRegion(
        Api::Region region, std::span<Api::CellValue> output) const noexcept override;
    [[nodiscard]] std::expected<Api::EditResult, Api::EditFailure> Apply(
        std::span<const Api::CellMutation> mutations) override;

  private:
    class Impl;
    std::unique_ptr<Impl> m_Impl;
};

}
