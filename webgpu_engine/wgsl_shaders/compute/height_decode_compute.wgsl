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


@group(0) @binding(0) var input_texture: texture_storage_2d<rgba8uint, read>;
@group(0) @binding(1) var output_texture: texture_storage_2d<r32float, write>;

@compute @workgroup_size(16, 16, 1)
fn computeMain(@builtin(global_invocation_id) id: vec3<u32>) {
    // Exit if thread id is outside the image dimensions
    let output_texture_size = textureDimensions(output_texture);
    if (id.x >= output_texture_size.x || id.y >= output_texture_size.y) {
        return;
    }

    // Load the RGBA8 color from the input texture
    let input: vec4<u32> = textureLoad(input_texture, vec2<i32>(id.xy));

    let scaling_factor: f32 = 8191.875 / 65535.0;
    let height_value: f32 = f32((input.r << 8) | input.g) * scaling_factor;
    
    // ToDo: cos(posy). Lets first evaluate wehter we actually need this. If so it makes sense to precaculate them on cpu side
    // for the tile bounds and then just interpolate them here based on xy.

    // Write the red channel value to the output texture
    textureStore(output_texture, vec2<i32>(id.xy), vec4<f32>(height_value, 0.0, 0.0, 0.0));
}
