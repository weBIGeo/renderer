/*****************************************************************************
 * weBIGeo
 * Copyright (C) 2023 Gerald Kimmersdorfer
 * Copyright (C) 2024 Adam Celarek
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

#include "filtering.wgsl"
#include "tile_util.wgsl"
#include "tile_hashmap.wgsl"

struct AreaOfInfluenceSettings {
    tile_id: vec4u,
    uv: vec4f,
}

// input
@group(0) @binding(0) var<storage> input_tile_ids: array<TileId>;
@group(0) @binding(1) var<storage> input_tile_bounds: array<vec4<f32>>;
@group(0) @binding(2) var<uniform> settings: AreaOfInfluenceSettings;

@group(0) @binding(3) var<storage> map_key_buffer: array<TileId>; // hash map key buffer
@group(0) @binding(4) var<storage> map_value_buffer: array<u32>; // hash map value buffer, contains texture array indices
@group(0) @binding(5) var input_normal_tiles: texture_2d_array<f32>; // normal tiles
@group(0) @binding(6) var input_normal_tiles_sampler: sampler;

// output
@group(0) @binding(7) var output_tiles: texture_storage_2d_array<rgba8unorm, write>; // influence tiles (output)

@compute @workgroup_size(1, 16, 16)
fn computeMain(@builtin(global_invocation_id) id: vec3<u32>) {
    // id.x  in [0, num_tiles]
    // id.yz in [0, ceil(texture_dimensions(output_tiles).xy / workgroup_size.yz) - 1]

    // exit if thread id is outside image dimensions (i.e. thread is not supposed to be doing any work)
    let output_texture_size = textureDimensions(output_tiles);
    if (id.y >= output_texture_size.x || id.z >= output_texture_size.y) {
        return;
    }
    // id.yz in [0, texture_dimensions(output_tiles) - 1]

    let tile_id = input_tile_ids[id.x];
    let bounds = input_tile_bounds[id.x];
    let input_texture_size = textureDimensions(input_normal_tiles);
    let quad_width: f32 = (bounds.z - bounds.x) / f32(input_texture_size.x - 1);
    let quad_height: f32 = (bounds.w - bounds.y) / f32(input_texture_size.y - 1);

    let col = id.y; // in [0, texture_dimension(output_tiles).x - 1]
    let row = id.z; // in [0, texture_dimension(output_tiles).y - 1]
    let uv = vec2f(f32(col), f32(input_texture_size.y - 1 - row)) / vec2f(output_texture_size - 1);
    
    let to_target_tile = vec2f(vec2i(settings.tile_id.xy) - vec2i(i32(tile_id.x), i32(tile_id.y)));
    let to_target_uv = settings.uv.xy - uv;
    let to_target_total = to_target_tile + to_target_uv;
    let total_distance = length(to_target_total);
    
    let overlay = select(0.0, 1.0, total_distance < 1.0);
    
    textureStore(output_tiles, vec2(col, row), id.x, vec4f(1.0, 0.0, 0.0, f32(overlay))); // incorrect
}
