/*****************************************************************************
 * weBIGeo
 * Copyright (C) 2024 Patrick Komon
 * Copyright (C) 2024 Gerald Kimmersdorfer
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
#include "ShaderModuleManager.h"
#include "TileManager.h"
#include "UniformBufferObjects.h"
#include "compute/nodes.h"
#include "nucleus/AbstractRenderWindow.h"
#include "nucleus/camera/AbstractDepthTester.h"
#include "nucleus/camera/Controller.h"
#include "nucleus/utils/ColourTexture.h"
#include "raii/BindGroup.h"
#include "raii/Sampler.h"
#include "raii/Texture.h"
#include "raii/TextureView.h"
#include <webgpu/webgpu.h>

class QOpenGLFramebufferObject;

namespace webgpu_engine {

class Window : public nucleus::AbstractRenderWindow, public nucleus::camera::AbstractDepthTester {
    Q_OBJECT
public:
    using ObtainWebGpuSurfaceFunc = std::function<WGPUSurface(WGPUInstance instance)>;
    using ImGuiWindowImplInitFunc = std::function<void()>;
    using ImGuiWindowImplNewFrameFunc = std::function<void()>;
    using ImGuiWindowImplShutdownFunc = std::function<void()>;

#ifdef ALP_WEBGPU_APP_ENABLE_IMGUI
    Window(ObtainWebGpuSurfaceFunc obtainWebGpuSurfaceFunc, ImGuiWindowImplInitFunc imguiWindowInitFunc,
        ImGuiWindowImplNewFrameFunc imguiWindowNewFrameFunc, ImGuiWindowImplShutdownFunc imguiWindowShutdownFunc);
#else
    Window(ObtainWebGpuSurfaceFunc obtainWebGpuSurfaceFunc);
#endif

    ~Window() override;

    void initialise_gpu() override;
    void resize_framebuffer(int w, int h) override;
    void paint(QOpenGLFramebufferObject* framebuffer = nullptr) override;

    [[nodiscard]] float depth(const glm::dvec2& normalised_device_coordinates) override;
    [[nodiscard]] glm::dvec3 position(const glm::dvec2& normalised_device_coordinates) override;
    void deinit_gpu() override;
    void set_aabb_decorator(const nucleus::tile_scheduler::utils::AabbDecoratorPtr&) override;
    void set_quad_limit(unsigned new_limit) override;
    [[nodiscard]] nucleus::camera::AbstractDepthTester* depth_tester() override;
    nucleus::utils::ColourTexture::Format ortho_tile_compression_algorithm() const override;
    void set_permissible_screen_space_error(float new_error) override;

    // Not happy with the following three in public, but we need to have access inside the lambdas.
    std::unique_ptr<raii::RawBuffer<glm::vec4>> m_position_readback_buffer;
    bool m_position_readback_done = true;
    glm::vec4 m_position_readback_result;

public slots:
    void update_camera(const nucleus::camera::Definition& new_definition) override;
    void update_debug_scheduler_stats(const QString& stats) override;
    void update_gpu_quads(const std::vector<nucleus::tile_scheduler::tile_types::GpuTileQuad>& new_quads, const std::vector<tile::Id>& deleted_quads) override;

private:
    void create_instance();
    void init_surface();
    void request_adapter();
    void request_device();
    void init_queue();
    void create_buffers();
    void create_depth_texture(uint32_t width, uint32_t height);
    void create_bind_groups();
    void create_swapchain(uint32_t w, uint32_t h);

    bool init_gui();
    void terminate_gui();
    void update_gui(WGPURenderPassEncoder render_pass);

    WGPURequiredLimits required_gpu_limits() const;

    // A helper function for the depth and position method.
    // ATTENTION: This function is synchronous and will hold rendering. Use with caution!
    // Note: Depth aswell as the position is saved in the gbuffer. In contrast to the gl version
    // we can directly readback the content of the position buffer and don't need the readback depth
    // buffer anymore. May actually increase performance as we don't need to fill the seperate buffer.
    glm::vec4 synchronous_position_readback(const glm::dvec2& normalised_device_coordinates);

private:
    WGPUInstance m_instance = nullptr;
    WGPUDevice m_device = nullptr;
    WGPUAdapter m_adapter = nullptr;
    WGPUSurface m_surface = nullptr;
    WGPUSwapChain m_swapchain = nullptr;
    WGPUTextureFormat m_swapchain_format = WGPUTextureFormat::WGPUTextureFormat_Undefined;
    WGPUTextureFormat m_depth_texture_format = WGPUTextureFormat::WGPUTextureFormat_Depth24Plus;

    WGPUQueue m_queue = nullptr;

    std::unique_ptr<ShaderModuleManager> m_shader_manager;
    std::unique_ptr<PipelineManager> m_pipeline_manager;

    std::unique_ptr<raii::Buffer<uboSharedConfig>> m_shared_config_ubo;
    std::unique_ptr<raii::Buffer<uboCameraConfig>> m_camera_config_ubo;

    std::unique_ptr<raii::BindGroup> m_shared_config_bind_group;
    std::unique_ptr<raii::BindGroup> m_camera_bind_group;
    std::unique_ptr<raii::BindGroup> m_compose_bind_group;

    ObtainWebGpuSurfaceFunc m_obtain_webgpu_surface_func;
    ImGuiWindowImplInitFunc m_imgui_window_init_func;
    ImGuiWindowImplNewFrameFunc m_imgui_window_new_frame_func;
    ImGuiWindowImplShutdownFunc m_imgui_window_shutdown_func;

    nucleus::camera::Definition m_camera;

    std::unique_ptr<raii::Texture> m_depth_texture;
    std::unique_ptr<raii::TextureView> m_depth_texture_view;

    std::unique_ptr<TileManager> m_tile_manager;

    FramebufferFormat m_gbuffer_format;
    std::unique_ptr<Framebuffer> m_gbuffer;
    std::unique_ptr<raii::Sampler> m_compose_sampler_filtering;
    std::unique_ptr<raii::Sampler> m_compose_sampler_nonfiltering;

    std::unique_ptr<Framebuffer> m_atmosphere_framebuffer;

    // ToDo: Swapchain should get a raii class and the size could be saved in there
    glm::vec2 m_swapchain_size = glm::vec2(0.0f);
    WGPUPresentMode m_swapchain_presentmode = WGPUPresentMode::WGPUPresentMode_Fifo;

    std::unique_ptr<NodeGraph> m_compute_graph;
};

} // namespace webgpu_engine
