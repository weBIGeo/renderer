/*****************************************************************************
 * weBIGeo
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

#include <SDL2/SDL.h>
#include <set>
#include <string>
#include <vector>
#include <webgpu/webgpu.h>

class TerrainRenderer;

namespace webgpu_engine {
class Window;
}

namespace webgpu_app {

class TerrainRenderer;

class GuiManager {

public:
    GuiManager(TerrainRenderer* terrain_renderer);

    void init(SDL_Window* window, WGPUDevice device, WGPUTextureFormat swapchainFormat, WGPUTextureFormat depthTextureFormat);
    void render(WGPURenderPassEncoder renderPass);
    void shutdown();
    bool want_capture_keyboard();
    bool want_capture_mouse();
    void on_sdl_event(SDL_Event& event);

private:
    SDL_Window* m_window;
    WGPUDevice m_device;
    TerrainRenderer* m_terrain_renderer = nullptr;
    bool m_show_nodeeditor = false;

    std::vector<std::string> m_camera_preset_names;
    int m_selected_camera_preset = 0;

    std::set<uint32_t> m_selected_timer = {};

    void draw();

    void install_fonts();

    void toggle_timer(uint32_t timer_id);
    bool is_timer_selected(uint32_t timer_id);
};

} // namespace webgpu_app
