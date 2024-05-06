/*****************************************************************************
 * weBIGeo
 * Copyright (C) 2024 Gerald Kimmersdorfer
 * Copyright (C) 2024 Patrick Komon
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *****************************************************************************/

#pragma once

#include "PipelineManager.h"
#include "nucleus/tile_scheduler/TileLoadService.h"
#include "nucleus/tile_scheduler/tile_types.h"
#include "nucleus/timing/CpuTimer.h"
#include "raii/Buffer.h"
#include "raii/TextureWithSampler.h"
#include <QByteArray>
#include <QObject>
#include <queue>
#include <vector>

namespace webgpu_engine {

struct GpuTileId {
    uint32_t x;
    uint32_t y;
    uint32_t zoomlevel;
};

struct RectangularTileRegion {
    glm::uvec2 min;
    glm::uvec2 max;
    unsigned int zoom_level;
    tile::Scheme scheme;

    std::vector<tile::Id> get_tiles() const;
};

/// Manages a set of tiles in GPU memory
/// Supports adding and removing tiles, reading back tiles into host memory
class ComputeTileStorage {
public:
    using ReadBackCallback = std::function<void(size_t layer_index, std::shared_ptr<QByteArray>)>;

    virtual ~ComputeTileStorage() = default;

    virtual void init() = 0;
    virtual void store(const tile::Id& id, std::shared_ptr<QByteArray> data) = 0;
    virtual void clear(const tile::Id& id) = 0;
    virtual std::vector<WGPUBindGroupEntry> create_bind_group_entries(const std::vector<uint32_t>& bindings) const = 0;
    virtual void read_back_async(size_t layer_index, ReadBackCallback callback) = 0;
};

class TextureArrayComputeTileStorage : public ComputeTileStorage {
public:
    struct ReadBackState {
        std::unique_ptr<raii::RawBuffer<char>> buffer;
        ReadBackCallback callback;
        size_t layer_index;
    };

public:
    TextureArrayComputeTileStorage(WGPUDevice device, const glm::uvec2& resolution, size_t capacity, WGPUTextureFormat format,
        WGPUTextureUsageFlags usage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst);

    void init() override;
    void store(const tile::Id& id, std::shared_ptr<QByteArray> data) override;
    void clear(const tile::Id& id) override;

    void read_back_async(size_t layer_index, ReadBackCallback callback) override;

    std::vector<WGPUBindGroupEntry> create_bind_group_entries(const std::vector<uint32_t>& bindings) const override;

private:
    WGPUDevice m_device;
    WGPUQueue m_queue;
    std::unique_ptr<raii::TextureWithSampler> m_texture_array;
    std::unique_ptr<raii::RawBuffer<GpuTileId>> m_tile_ids;
    glm::uvec2 m_resolution;
    size_t m_capacity;

    std::vector<tile::Id> m_layer_index_to_tile_id;
    std::queue<ReadBackState> m_read_back_states;
};

class ComputeController : public QObject {
    Q_OBJECT

public:
    ComputeController(WGPUDevice device, const PipelineManager& pipeline_manager);
    ~ComputeController() = default;

    void request_tiles(const RectangularTileRegion& region);
    void run_pipeline();

    // write tile data to files only for debugging, writes next to app.exe
    void write_output_tiles(const std::filesystem::path& dir = ".") const;

    float get_last_tile_request_timing();
    float get_last_pipeline_run_timing();

public slots:
    void on_single_tile_received(const nucleus::tile_scheduler::tile_types::TileLayer& tile);

signals:
    void tiles_requested();
    void tiles_received();
    void pipeline_run_queued();
    void pipeline_done();

private:
    const size_t m_max_num_tiles = 256;
    const glm::uvec2 m_input_tile_resolution = { 65, 65 };
    const glm::uvec2 m_output_tile_resolution = { 256, 256 };

    size_t m_num_tiles_received = 0;
    size_t m_num_tiles_requested = 0;

    const PipelineManager* m_pipeline_manager;
    WGPUDevice m_device;
    WGPUQueue m_queue;
    std::unique_ptr<nucleus::tile_scheduler::TileLoadService> m_tile_loader;

    std::unique_ptr<raii::BindGroup> m_compute_bind_group;

    std::unique_ptr<ComputeTileStorage> m_input_tile_storage;
    std::unique_ptr<ComputeTileStorage> m_output_tile_storage;

    nucleus::timing::CpuTimer m_tile_request_timer;
    nucleus::timing::CpuTimer m_pipeline_run_timer;
};

} // namespace webgpu_engine
