/*****************************************************************************
 * weBIGeo
 * Copyright (C) 2022 Adam Celarek
 * Copyright (C) 2023 Gerald Kimmersdorfer
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

#include "shared_config.wgsl"
#include "hashing.wgsl"
#include "camera_config.wgsl"
#include "encoder.wgsl"
#include "tile_util.wgsl"
#include "tile_hashmap.wgsl"
#include "normals_util.wgsl"
#include "snow.wgsl"

@group(0) @binding(0) var<uniform> config: shared_config;

@group(1) @binding(0) var<uniform> camera: camera_config;

@group(2) @binding(0) var<uniform> n_edge_vertices: i32;
@group(2) @binding(1) var height_texture: texture_2d_array<u32>;
@group(2) @binding(2) var height_sampler: sampler;
@group(2) @binding(3) var ortho_texture: texture_2d_array<f32>;
@group(2) @binding(4) var ortho_sampler: sampler;

@group(3) @binding(0) var<storage> normal_hashmap_key_buffer: array<TileId>; // hash map key buffer
@group(3) @binding(1) var<storage> normal_hashmap_value_buffer: array<u32>; // hash map value buffer, contains texture array indices
@group(3) @binding(2) var normals_texture: texture_2d_array<f32>; // overlay tiles

@group(3) @binding(3) var<storage> overlay_hashmap_key_buffer: array<TileId>; // hash map key buffer
@group(3) @binding(4) var<storage> overlay_hashmap_value_buffer: array<u32>; // hash map value buffer, contains texture array indices
@group(3) @binding(5) var overlay_texture: texture_2d_array<f32>; // overlay tiles

struct VertexIn {
    @location(0) bounds: vec4f,
    @location(1) texture_layer: i32,
    @location(2) tileset_id: i32,
    @location(3) tileset_zoomlevel: i32,
    @location(4) tile_id: vec4<u32>,
}

struct VertexOut {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
    @location(1) pos_cws: vec3f,
    @location(2) normal: vec3f,
    @location(3) @interpolate(flat) texture_layer: i32,
    @location(4) color: vec3f,
    @location(5) @interpolate(flat) tile_id: vec3<u32>,
}

struct FragOut {
    @location(0) albedo: u32,
    @location(1) position: vec4f,
    @location(2) normal_enc: vec2u,
    @location(3) overlay: u32,
}

fn camera_world_space_position(
    vertex_index: u32,
    bounds: vec4f,
    texture_layer: i32,
    uv: ptr<function, vec2f>,
    n_quads_per_direction: ptr<function, f32>,
    quad_width: ptr<function, f32>,
    quad_height: ptr<function, f32>,
    altitude_correction_factor: ptr<function, f32>
) -> vec3f {
    let n_quads_per_direction_int = n_edge_vertices - 1;
    *n_quads_per_direction = f32(n_quads_per_direction_int);
    *quad_width = (bounds.z - bounds.x) / (*n_quads_per_direction);
    *quad_height = (bounds.w - bounds.y) / (*n_quads_per_direction);

    var row: i32 = i32(vertex_index) / n_edge_vertices;
    var col: i32 = i32(vertex_index) - (row * n_edge_vertices);
    let curtain_vertex_id = i32(vertex_index) - n_edge_vertices * n_edge_vertices;
    if (curtain_vertex_id >= 0) {
        if (curtain_vertex_id < n_edge_vertices) {
            row = (n_edge_vertices - 1) - curtain_vertex_id;
            col = (n_edge_vertices - 1);
        }
        else if (curtain_vertex_id >= n_edge_vertices && curtain_vertex_id < 2 * n_edge_vertices - 1) {
            row = 0;
            col = (n_edge_vertices - 1) - (curtain_vertex_id - n_edge_vertices) - 1;
        }
        else if (curtain_vertex_id >= 2 * n_edge_vertices - 1 && curtain_vertex_id < 3 * n_edge_vertices - 2) {
            row = curtain_vertex_id - 2 * n_edge_vertices + 2;
            col = 0;
        }
        else {
            row = (n_edge_vertices - 1);
            col = curtain_vertex_id - 3 * n_edge_vertices + 3;
        }
    }
    // Note: for higher zoom levels it would be enough to calculate the altitude_correction_factor on cpu
    // for lower zoom levels we could bake it into the texture.
    // but there was no measurable difference despite the cos and atan, so leaving as is for now.
    let var_pos_cws_y: f32 = f32(n_quads_per_direction_int - row) * f32(*quad_width) + bounds.y;
    let pos_y: f32 = var_pos_cws_y + camera.position.y;
    *altitude_correction_factor = 0.125 / cos(y_to_lat(pos_y)); // https://github.com/AlpineMapsOrg/renderer/issues/5

    *uv = vec2f(f32(col) / (*n_quads_per_direction), f32(row) / (*n_quads_per_direction));
    let altitude_tex = f32(textureLoad(height_texture, vec2i(col, row), texture_layer, 0).r);
    let adjusted_altitude: f32 = altitude_tex * (*altitude_correction_factor);

    var var_pos_cws = vec3f(f32(col) * (*quad_width) + bounds.x, var_pos_cws_y, adjusted_altitude - camera.position.z);

    if (curtain_vertex_id >= 0) {
        // TODO implement preprocessor constants in shader
        //float curtain_height = CURTAIN_REFERENCE_HEIGHT;

        var curtain_height = f32(1000);
// TODO implement preprocessor if in shader
/*#if CURTAIN_HEIGHT_MODE == 1
        let dist_factor = clamp(length(var_pos_cws) / 100000.0, 0.2, 1.0);
        curtain_height *= dist_factor;
#endif*/
        var_pos_cws.z = var_pos_cws.z - curtain_height;
    }

    return var_pos_cws;
}

fn normal_by_fragment_position_interpolation(pos_cws: vec3<f32>) -> vec3<f32> {
    let dFdxPos = dpdy(pos_cws);
    let dFdyPos = dpdx(pos_cws);
    return normalize(cross(dFdxPos, dFdyPos));
}

@vertex
fn vertexMain(@builtin(vertex_index) vertex_index: u32, vertex_in: VertexIn) -> VertexOut {
    var uv: vec2f;
    var n_quads_per_direction: f32;
    var quad_width: f32;
    var quad_height: f32;
    var altitude_correction_factor: f32;
    let var_pos_cws = camera_world_space_position(vertex_index, vertex_in.bounds, vertex_in.texture_layer, &uv, &n_quads_per_direction, &quad_width, &quad_height, &altitude_correction_factor);

    let pos = vec4f(var_pos_cws, 1);
    let clip_pos = camera.view_proj_matrix * pos;

    var vertex_out: VertexOut;
    vertex_out.position = clip_pos;
    vertex_out.uv = uv;
    vertex_out.pos_cws = var_pos_cws;

    vertex_out.normal = vec3f(0.0);
    if (config.normal_mode == 2) {
        vertex_out.normal = normal_by_finite_difference_method(uv, quad_width, quad_height, altitude_correction_factor, vertex_in.texture_layer, height_texture);
    }
    vertex_out.texture_layer = vertex_in.texture_layer;

    var vertex_color = vec3f(0.0);
    if (config.overlay_mode == 2) {
        vertex_color = color_from_id_hash(u32(vertex_in.tileset_id));
    } else if (config.overlay_mode == 3) {
        vertex_color = color_from_id_hash(u32(vertex_in.tileset_zoomlevel));
    } else if (config.overlay_mode == 4) {
        vertex_color = color_from_id_hash(u32(vertex_index));
    }
    vertex_out.color = vertex_color;
    vertex_out.tile_id = vertex_in.tile_id.xyz;
    return vertex_out;
}

@fragment
fn fragmentMain(vertex_out: VertexOut) -> FragOut {
    var albedo = textureSample(ortho_texture, ortho_sampler, vertex_out.uv, vertex_out.texture_layer).rgb;
    var dist = length(vertex_out.pos_cws);

    var frag_out: FragOut;
    

    let tile_id = TileId(vertex_out.tile_id.x, vertex_out.tile_id.y, vertex_out.tile_id.z, 4294967295u);
    var normal = vertex_out.normal;
    if (config.normal_mode != 0) {
        if (config.normal_mode == 1) {
            normal = normal_by_fragment_position_interpolation(vertex_out.pos_cws);
        }

        // replace per vertex normals with better normals, if present
        var texure_array_index: u32;
        let found = get_texture_array_index(tile_id, &texure_array_index, &normal_hashmap_key_buffer, &normal_hashmap_value_buffer);
        
        // remap texture coordinates to skip first and last half texel (so uv grid spans only texel centers)
        let normal_texture_size = textureDimensions(normals_texture);
        let normal_uv = vertex_out.uv * (vec2f(normal_texture_size - 1) / vec2f(normal_texture_size)) + 1f / (2f * vec2f(normal_texture_size));
        let normal_texture_texel_value = textureSample(normals_texture, ortho_sampler, normal_uv, texure_array_index).xyzw;

        if (found && normal_texture_texel_value.w != 0.0f) {
            normal = normal_texture_texel_value.xyz * 2.0 - 1.0;
            dist = -1.0; // temporary such that we know in compose if we are inside the precalculated area
        }

        frag_out.normal_enc = octNormalEncode2u16(normal);
    }

    // HANDLE OVERLAYS (and mix it with the albedo color) THAT CAN JUST BE DONE IN THIS STAGE
    // NOTE: Performancewise its generally better to handle overlays in the compose step! (overdraw)
    var overlay_color = vec4f(0.0);
    if (config.overlay_mode > 0u && config.overlay_mode < 100u) {
        if (config.overlay_mode == 1) {
            overlay_color = vec4f(normal * 0.5 + 0.5, 1.0);
        } else if (config.overlay_mode == 99) { // compute overlay
            //TODO we should probably write overlay color into a separate gbuffer texture and do blending in compose shader (?) 
            
            var texure_array_index: u32;
            let found = get_texture_array_index(tile_id, &texure_array_index, &overlay_hashmap_key_buffer, &overlay_hashmap_value_buffer);

             // remap texture coordinates to skip first and last half texel (so uv grid spans only texel centers)
            let overlay_texture_size = textureDimensions(overlay_texture);
            let overlay_uv = vertex_out.uv * (vec2f(overlay_texture_size - 1) / vec2f(overlay_texture_size)) + 1f / (2f * vec2f(overlay_texture_size));

            // textureSample needs to happen in uniform control flow
            // therefore: if texture was found, sample correct texture array index, otherwise sample from texture 0
            let sampled_overlay_color = textureSample(overlay_texture, ortho_sampler, vertex_out.uv, texure_array_index).rgba;
    
            if (found) {
                overlay_color = sampled_overlay_color;
            }
        }
        //albedo = mix(albedo, overlay_color.xyz, config.overlay_strength * overlay_color.w);
    }
    frag_out.overlay = pack4x8unorm(overlay_color);
    frag_out.albedo = pack4x8unorm(vec4f(albedo, 1.0));

    frag_out.position = vec4f(vertex_out.pos_cws, dist);

    return frag_out;
}
