/*****************************************************************************
 * weBIGeo
 * Copyright (C) 2022 Gerald Kimmersdorfer
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

#include "util/shared_config.wgsl"
#include "util/camera_config.wgsl"
#include "util/atmosphere.wgsl"
#include "util/encoder.wgsl"
#include "util/snow.wgsl"
#include "util/tile_util.wgsl"

#include "screen_pass_vert.wgsl"

@group(0) @binding(0) var<uniform> conf : shared_config;
@group(1) @binding(0) var<uniform> camera : camera_config;

@group(2) @binding(0) var albedo_texture : texture_2d<u32>;
@group(2) @binding(1) var position_texture : texture_2d<f32>;
@group(2) @binding(2) var normal_texture : texture_2d<u32>;
@group(2) @binding(3) var atmosphere_texture : texture_2d<f32>;
@group(2) @binding(4) var overlay_texture : texture_2d<u32>;

@group(2) @binding(5) var<uniform> image_overlay_settings: ImageOverlaySettings;
@group(2) @binding(6) var image_overlay_texture: texture_2d<f32>;
@group(2) @binding(7) var image_overlay_sampler: sampler;

struct ImageOverlaySettings {
    aabb_min: vec2f,
    aabb_max: vec2f,
    alpha: f32,
    padding0: f32,
    padding1: f32,
    padding2: f32,
}

// Calculates the diffuse and specular illumination contribution for the given
// parameters according to the Blinn-Phong lighting model.
// All parameters must be normalized.
fn calc_blinn_phong_contribution(
    toLight: vec3<f32>, 
    toEye: vec3<f32>, 
    normal: vec3<f32>, 
    diffFactor: vec3<f32>, 
    specFactor: vec3<f32>, 
    specShininess: f32
) -> vec3<f32> {
    let nDotL: f32 = max(0.0, dot(normal, toLight)); // Lambertian coefficient
    let h: vec3<f32> = normalize(toLight + toEye);
    let nDotH: f32 = max(0.0, dot(normal, h));
    let specPower: f32 = pow(nDotH, specShininess);
    let diffuse: vec3<f32> = diffFactor * nDotL; // Component-wise product
    let specular: vec3<f32> = specFactor * specPower;
    return diffuse + specular;
}

// Calculates the Blinn-Phong illumination for the given fragment
fn calculate_illumination(
    albedo: vec3<f32>, 
    eyePos: vec3<f32>, 
    fragPos: vec3<f32>, 
    fragNorm: vec3<f32>, 
    dirLight: vec4<f32>, 
    ambLight: vec4<f32>, 
    dirDirection: vec3<f32>, 
    material: vec4<f32>, 
    ao: f32, 
    shadow_term: f32
) -> vec3<f32> {
    let dirColor: vec3<f32> = dirLight.rgb * dirLight.a;
    let ambColor: vec3<f32> = ambLight.rgb * ambLight.a;
    let ambient: vec3<f32> = material.r * albedo;
    let diff: vec3<f32> = material.g * albedo;
    let spec: vec3<f32> = vec3<f32>(material.b);
    let shini: f32 = material.a;

    let ambientIllumination: vec3<f32> = ambient * ambColor * ao;

    let toLightDirWS: vec3<f32> = -normalize(dirDirection);
    let toEyeNrmWS: vec3<f32> = normalize(eyePos - fragPos);
    let diffAndSpecIllumination: vec3<f32> = dirColor * calc_blinn_phong_contribution(toLightDirWS, toEyeNrmWS, fragNorm, diff, spec, shini);

    return ambientIllumination + diffAndSpecIllumination * (1.0 - shadow_term);
}


@fragment
fn fragmentMain(vertex_out : VertexOut) -> @location(0) vec4f {
    let tci : vec2<u32> = vec2u(vertex_out.texcoords * camera.viewport_size);

    var albedo: vec3f = unpack4x8unorm(textureLoad(albedo_texture, tci, 0).r).xyz;
    let pos_dist = textureLoad(position_texture, tci, 0);
    let encoded_normal = textureLoad(normal_texture, tci, 0).xy;

    let pos_cws = pos_dist.xyz;
    let dist = length(pos_cws); // pos_dist.w
    let tile_dist = pos_dist.w;
    var alpha = 0.0;
    if (dist > 0) {
        alpha = calculate_falloff(dist, 300000.0, 600000.0);
    }

    let normal = octNormalDecode2u16(encoded_normal);

    var shaded_color = vec3f(0.0);
    
    var amb_occlusion = 1.0;
    // Gather ambient occlusion from ssao texture
    if (bool(conf.ssao_enabled)) {
        //TODO
        //amb_occlusion = texture(texin_ssao, texcoords).r;
    }
    
    let sampled_shadow_layer: i32 = -1;
    
    let origin = camera.position.xyz;
    let pos_ws = pos_cws + origin;

    // sampling from texture needs to happen in uniform control flow, therefore this is outside the if
    // later, the sampled value is only used if we are in the overlay region (specified in image overlay settings uniform)
    let image_overlay_uv = (pos_ws.xy - image_overlay_settings.aabb_min) / (image_overlay_settings.aabb_max - image_overlay_settings.aabb_min);
    let image_overlay_color = textureSample(image_overlay_texture, image_overlay_sampler, vec2f(image_overlay_uv.x, 1 - image_overlay_uv.y)); 

    // Don't do shading if not visible anyway and also don't for pixels where there is no geometry (depth==0.0)
    if (dist > 0.0) {
        let ray_direction = pos_cws / dist;
        var material_light_response = conf.material_light_response;

        //TODO (unused in original code?)
        //let light_through_atmosphere = calculate_atmospheric_light(origin / 1000.0, ray_direction, dist / 1000.0, albedo, 10);

        var shadow_term = 0.0;
        if (bool(conf.csm_enabled)) {
            //TODO
            //shadow_term = csm_shadow_term(vec4(pos_cws, 1.0), normal, sampled_shadow_layer);
        }

        if (bool(conf.snow_settings_angle.x)) {
            // note: for now we use fragment snow with overlays (trajectories on top of fragment snow)
            //   for precomputed snow, we would want to disable fragment shader snow in the area where overlay tiles are available
            //   for this behavior, comment in the following if  
            
            //if (tile_dist >= 0.0f) { // -1 if snow is already calculated in tile stage
            let overlay_color: vec4f = overlay_snow(normal, pos_ws, conf.snow_settings_angle, conf.snow_settings_alt);
            material_light_response.z += conf.snow_settings_alt.w * overlay_color.a;
            albedo = mix(albedo, overlay_color.rgb, overlay_color.a);
            //}
        }

        // NOTE: PRESHADING OVERLAY ONLY APPLIED ON TILES NOT ON BACKGROUND!!!
        if (!bool(conf.overlay_postshading_enabled)) {
            var overlay_color = vec4f(0.0);
            if (conf.overlay_mode == 100u) {
                overlay_color = vec4f(normal * 0.5 + 0.5, 1.0);
            } else if (conf.overlay_mode == 101u) {
                //TODO implement
                //overlay_color = overlay_steepness(normal, dist);
            } else if (conf.overlay_mode == 102u) {
                //TODO implement
                //overlay_color = vec4f(amb_occlusion, amb_occlusion, amb_occlusion, 1.0);
            } else if (conf.overlay_mode == 103u) {
                //TODO implement
                //overlay_color = vec4(color_from_id_hash(uint(sampled_shadow_layer)), 1.0);
            } else if ((conf.overlay_mode == 1u) || (conf.overlay_mode == 99u)) {
                overlay_color = unpack4x8unorm(textureLoad(overlay_texture, tci, 0).r);
            }
            overlay_color.a *= conf.overlay_strength;
            albedo = mix(albedo, overlay_color.rgb, overlay_color.a);
        }

        shaded_color = albedo;
        if (bool(conf.phong_enabled)) {
            shaded_color = calculate_illumination(shaded_color, origin, pos_ws, normal, conf.sun_light, conf.amb_light, conf.sun_light_dir.xyz, material_light_response, amb_occlusion, shadow_term);
        }
        shaded_color = calculate_atmospheric_light(origin / 1000.0, ray_direction, dist / 1000.0, shaded_color, 10);
        shaded_color = max(vec3(0.0), shaded_color);    
    }

    // Blend with atmospheric background:
    let atmospheric_color = textureLoad(atmosphere_texture, vec2u(0,tci.y), 0).rgb;
    //let atmospheric_color = textureSample(atmosphere_texture, compose_sampler_filtering, vertex_out.texcoords.xy).rgb;
    var out_Color = vec4f(mix(atmospheric_color, shaded_color, alpha), 1.0);

    if (dist > 0.0 && all(pos_ws.xy >= image_overlay_settings.aabb_min) && all(pos_ws.xy <= image_overlay_settings.aabb_max)) {
        //return vec4f(1, 0, 0, 1);
        out_Color = vec4(mix(out_Color.rgb, image_overlay_color.rgb, image_overlay_settings.alpha), out_Color.a);
    }

    if (bool(conf.overlay_postshading_enabled)) {
        var overlay_color = vec4f(0.0);
        if (conf.overlay_mode == 100u) {
            overlay_color = vec4f(normal * 0.5 + 0.5, 1.0);
        } else if (conf.overlay_mode == 101u) {
            //TODO implement
            //overlay_color = overlay_steepness(normal, dist);
        } else if (conf.overlay_mode == 102u) {
            //TODO implement
            //overlay_color = vec4f(amb_occlusion, amb_occlusion, amb_occlusion, 1.0);
        } else if (conf.overlay_mode == 103u) {
            //TODO implement
            //overlay_color = vec4(color_from_id_hash(uint(sampled_shadow_layer)), 1.0);
        } else if ((conf.overlay_mode == 1u) || (conf.overlay_mode == 99u)) {
            overlay_color = unpack4x8unorm(textureLoad(overlay_texture, tci, 0).r);
        }
        overlay_color.a *= conf.overlay_strength;
        out_Color = vec4(mix(out_Color.rgb, overlay_color.rgb, overlay_color.a), out_Color.a);
    }

/*
    // OVERLAY SHADOW MAPS
    if (bool(conf.overlay_shadowmaps_enabled)) {
        highp float wsize = 1.0 / float(SHADOW_CASCADES);
        highp float invwsize = 1.0/wsize;
        if (texcoords.x < wsize) {
            for (int i = 0 ; i < SHADOW_CASCADES; i++)
            {
                if (texcoords.y < wsize * float(i+1)) {
                    highp float val = sample_shadow_texture(i, (texcoords - vec2(0.0, wsize*float(i))) * invwsize);
                    out_Color = vec4(val, val, val, 1.0);
                    break;
                }
            }
        }
    }

    // == HEIGHT LINES ==============
    if (bool(conf.height_lines_enabled) && dist > 0.0) {
        highp float alpha_line = 1.0 - min((dist / 20000.0), 1.0);
        highp float line_width = (2.0 + dist / 5000.0) * 5.0;
        // Calculate steepness based on fragment normal (this alone gives woobly results)
        highp float steepness = (1.0 - dot(normal, vec3(0.0,0.0,1.0))) / 2.0;
        // Discretize the steepness -> Doesnt work
        //float steepness_discretized = int(steepness * 10.0f) / 10.0f;
        line_width = line_width * max(0.01,steepness);
        if (alpha_line > 0.05)
        {
            highp float alt = pos_cws.z + camera.position.z;
            highp float alt_rest = (alt - float(int(alt / 100.0)) * 100.0) - line_width / 2.0;
            if (alt_rest < line_width) {
                out_Color = vec4(mix(out_Color.rgb, vec3(out_Color.r - 0.2, out_Color.g - 0.2, out_Color.b - 0.2), alpha_line), out_Color.a);
            }
        }
    }

    */

    return out_Color;
}
