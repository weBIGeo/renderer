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
    target_point: vec4f,
    reference_point: vec4f,
    radius: f32,
    padding1: f32,
    padding2: f32,
    padding3: f32,
}

// input
@group(0) @binding(0) var<storage> input_tile_ids: array<TileId>; // tiles ids to process
@group(0) @binding(1) var<storage> input_tile_bounds: array<vec4<f32>>; // pre-computed tile bounds per tile id (in world space, relative to reference point)
@group(0) @binding(2) var<uniform> settings: AreaOfInfluenceSettings;

@group(0) @binding(3) var<storage> map_key_buffer: array<TileId>; // hash map key buffer
@group(0) @binding(4) var<storage> map_value_buffer: array<u32>; // hash map value buffer, contains texture array indices
@group(0) @binding(5) var input_normal_tiles: texture_2d_array<f32>; // normal tiles
@group(0) @binding(6) var input_normal_tiles_sampler: sampler; // normal sampler
@group(0) @binding(7) var input_height_tiles: texture_2d_array<f32>; // height tiles
@group(0) @binding(8) var input_height_tiles_sampler: sampler; // height sampler

@group(0) @binding(9) var<storage> output_tiles_map_key_buffer: array<TileId>; // hash map key buffer for output tiles
@group(0) @binding(10) var<storage> output_tiles_map_value_buffer: array<u32>; // hash map value buffer, contains texture array indice for output tiles

// output
@group(0) @binding(11) var output_tiles: texture_storage_2d_array<rgba8unorm, write>; // influence tiles (output)

fn get_gradient(normal: vec3f) -> vec3f {
    let up = vec3f(0, 0, 1);
    let right = cross(up, normal);
    let gradient = cross(right, normal);
    return gradient;
}

fn should_paint(col: u32, row: u32, tile_id: TileId) -> bool {
    return (col % 16 == 0) && (row % 16 == 0);
    //return (col % 16 == 0) && (row % 16 == 0) && (tile_id.x == 140386 + 1) && (tile_id.y == 169805 + 1);
    //return (col == 0) && (row == 0) && (tile_id.x == 140386 + 1) && (tile_id.y == 169805 + 1);
    //return (col == 64) && (row == 64) && (tile_id.x == 140386 + 1) && (tile_id.y == 169805 + 1);
}

fn gradient_overlay(id: vec3<u32>) {
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
    let tile_width: f32 = (bounds.z - bounds.x);
    let tile_height: f32 = (bounds.w - bounds.y);

    let col = id.y; // in [0, texture_dimension(output_tiles).x - 1]
    let row = id.z; // in [0, texture_dimension(output_tiles).y - 1]
    let uv = vec2f(f32(col), f32(row)) / vec2f(output_texture_size - 1);

    // read normal
    var texture_array_index: u32;
    let found = get_texture_array_index(tile_id, &texture_array_index, &map_key_buffer, &map_value_buffer);
    if (!found) {
        return;
    }
    let normal = bilinear_sample_vec4f(input_normal_tiles, input_normal_tiles_sampler, uv, texture_array_index).xyz * 2 - 1;
    let gradient = get_gradient(normal);
    textureStore(output_tiles, vec2u(col, row), id.x, vec4f(gradient, 1));
}

// draws traces within a single tile
fn traces_overlay(id: vec3<u32>) {
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
    let tile_width: f32 = (bounds.z - bounds.x);
    let tile_height: f32 = (bounds.w - bounds.y);

    let col = id.y; // in [0, texture_dimension(output_tiles).x - 1]
    let row = id.z; // in [0, texture_dimension(output_tiles).y - 1]
    let uv = vec2f(f32(col), f32(row)) / vec2f(output_texture_size - 1);

    if (!should_paint(col, row, tile_id)) {
        return;
    }

    let STEP_LENGTH: f32 = 1.0 / f32(input_texture_size.x - 1);
    let MAX_NUM_STEPS: i32 = 64;

    var uv_space_offset = vec2f(0, 0); // offset from original world position in uv coords
    for (var i: i32 = 0; i < MAX_NUM_STEPS; i++) {
        // calculate tile id and uv coordinates
        let new_uv = fract(uv + uv_space_offset); //TODO this is actually never 1; also we might need some offset because of tile overlap (i think)
        let uv_space_tile_offset = vec2i(floor(uv + uv_space_offset));
        let world_space_tile_offset = vec2i(uv_space_tile_offset.x, -uv_space_tile_offset.y); // world space y is opposite to uv space y, therefore invert y
        let new_tile_coords = vec2i(i32(tile_id.x), i32(tile_id.y)) + world_space_tile_offset;
        let new_tile_id = TileId(u32(new_tile_coords.x), u32(new_tile_coords.y), tile_id.zoomlevel, 0);

        // paint trace point
        let output_coords = vec2u(new_uv * vec2f(output_texture_size));
        var output_texture_array_index: u32;
        let found_output_tile = get_texture_array_index(new_tile_id, &output_texture_array_index, &output_tiles_map_key_buffer, &output_tiles_map_value_buffer);
        if (found_output_tile) {
            let color = vec3(f32(col) / f32(output_texture_size.x), f32(row) / f32(output_texture_size.y), 0.0);
            //let color = vec3(1.0 - f32(i) / f32(MAX_NUM_STEPS), 0.0, 0.0);
            textureStore(output_tiles, output_coords, output_texture_array_index, vec4f(color, 1.0));
        }

        // read normal
        var texture_array_index: u32;
        let found = get_texture_array_index(new_tile_id, &texture_array_index, &map_key_buffer, &map_value_buffer);
        if (!found) {
            // moved to a tile where we don't have any input data, discard
            break;
        }
        let normal = bilinear_sample_vec4f(input_normal_tiles, input_normal_tiles_sampler, new_uv, texture_array_index).xyz * 2 - 1;
        let gradient = get_gradient(normal);

        // step along gradient
        let uv_space_gradient = normalize(vec2f(gradient.x, -gradient.y));
        uv_space_offset = uv_space_offset + STEP_LENGTH * uv_space_gradient;
    }

    // overpaint start point
    textureStore(output_tiles, vec2u(col, row), id.x, vec4f(0.0, 0.0, 1.0, 1.0));
}

fn area_of_influence_overlay(id: vec3<u32>) {
    //TODO
    
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
    let tile_width: f32 = (bounds.z - bounds.x);
    let tile_height: f32 = (bounds.w - bounds.y);

    let col = id.y; // in [0, texture_dimension(output_tiles).x - 1]
    let row = id.z; // in [0, texture_dimension(output_tiles).y - 1]
    let uv = vec2f(f32(col), f32(row)) / vec2f(output_texture_size - 1);

    // calculate distance in world coordinates
    let pos_x = uv.x * f32(tile_width) + bounds.x;
    let pos_y = (1 - uv.y) * f32(tile_height) + bounds.y;
    //let to_target = settings.target_point.xy - vec2f(pos_x, pos_y);
    //let distance_to_target = length(to_target);
    //overlay = select(vec4f(0, 0, 0, 0), vec4f(1, 0, 0, 1), distance_to_target < settings.radius);
    //textureStore(output_tiles, vec2(col, row), id.x, overlay);
    //return;

    // for max number of steps
    let STEP_LENGTH: f32 = 1 / f32(input_texture_size.x - 1);
    let MAX_NUM_STEPS: i32 = 128;

    var overlay = vec4f(0);
    var uv_offset = vec2f(0, 0);
    for (var i: i32 = 0; i < MAX_NUM_STEPS; i++) {
        // calculate tile id and uv coordinates and look up normal
        let new_uv = fract(uv + vec2f(uv_offset.x, -uv_offset.y));
        var tile_offset = vec2i(floor(uv + uv_offset));
        let new_tile_coords = vec2i(i32(tile_id.x), i32(tile_id.y)) + vec2i(tile_offset.x, tile_offset.y);
        let new_tile_id = TileId(u32(new_tile_coords.x), u32(new_tile_coords.y), tile_id.zoomlevel, 0);

        // check if target reached
        let new_pos_x = pos_x + uv_offset.x * f32(tile_width);
        let new_pos_y = pos_y + uv_offset.y * f32(tile_height);
        let to_target = settings.target_point.xy - vec2f(new_pos_x, new_pos_y);
        /*if (length(to_target) < settings.radius) {
            overlay = vec4f(1, 0, 0, 1);
            break;
        }*/

        // read normal
        var texture_array_index: u32;
        let found = get_texture_array_index(new_tile_id, &texture_array_index, &map_key_buffer, &map_value_buffer);
        if (!found) {
            // moved to a tile where we don't have any data, discard
            //overlay = vec4f(0, 1, 0, 1);
            break;
        }
        let normal = bilinear_sample_vec4f(input_normal_tiles, input_normal_tiles_sampler, new_uv, texture_array_index).xyz * 2 - 1;
        let gradient = get_gradient(normal);

        // step along gradient
        uv_offset = uv_offset + STEP_LENGTH * gradient.xy;
    }
}

@compute @workgroup_size(1, 16, 16)
fn computeMain(@builtin(global_invocation_id) id: vec3<u32>) {
    traces_overlay(id);
    //gradient_overlay(id);
}