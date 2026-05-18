#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : enable

layout(set = 0, binding = 0, std430) buffer AccumulationBuffer { vec4 accumulation_buffer[]; };

layout(set = 0, binding = 1, std140) uniform Camera {
    vec4 pos;
    vec4 forward;
    vec4 right;
    vec4 up;
    uint frame_count;
    float sun_intensity;
    float sky_intensity;
    float exposure;
    uint path_tracing_enabled;
    uint max_bounces;
    uint sunlight_enabled;
    uint direct_lighting_enabled;
    float fov_y;
    float sun_angular_radius;
    float indirect_strength;
    uint environment_direct_samples;
} camera;

layout(set = 0, binding = 2, std430) buffer VarianceBuffer { uint variance_buffer[]; };
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D output_color;
layout(set = 0, binding = 4, std430) buffer DepthNormalBuffer { uvec2 depth_normal_buffer[]; };
layout(set = 0, binding = 5, std430) buffer WorldPositionBuffer { uvec2 world_position_buffer[]; };
layout(set = 0, binding = 10, std430) readonly buffer MeshMaterials { vec4 mesh_materials[]; };

layout(set = 0, binding = 11, std140) uniform MeshParams {
    uint vertex_count;
    uint triangle_count;
    uint bvh_node_count;
    uint material_count;
    uint enabled;
    uint sphere_count;
    uint primitive_count;
    uint instance_count;
    uint light_count;
    float emissive_total_area;
    uint mesh_count;
    uint local_vertex_count;
    uint local_index_count;
    uint local_bvh_node_count;
    uint local_triangle_count;
    uint tlas_node_count;
    uint tlas_instance_index_count;
    uint _padding2;
    uint _padding3;
    uint _padding4;
    uint _padding5;
    uint _padding6;
} mesh_params;

layout(set = 0, binding = 12) uniform texture2D env_map;
layout(set = 0, binding = 13) uniform sampler env_sampler;
layout(set = 0, binding = 14, std430) readonly buffer EnvCdfRows { float env_cdf_rows[]; };
layout(set = 0, binding = 15, std430) readonly buffer EnvCdfCols { float env_cdf_cols[]; };
layout(set = 0, binding = 16, std140) uniform EnvParams {
    uint enabled;
    float intensity;
    float rotation;
    uint width;
    uint height;
    float background_intensity;
    uint procedural;
    float _pad2;
    float inv_total_lum;
} env_params;

layout(set = 0, binding = 17, std430) readonly buffer SceneSpheres { vec4 scene_spheres[]; };

layout(set = 0, binding = 18, std140) uniform RendererDebug {
    uint view;
    uint flags;
    float scale;
    float _debug_pad0;
} debug_params;

layout(set = 0, binding = 19) uniform texture2D material_textures[128];
layout(set = 0, binding = 23) uniform sampler material_samplers[128];

struct PrimitiveRecord {
    uvec4 index_data;
    uvec4 metadata;
};
layout(set = 0, binding = 21, std430) readonly buffer ScenePrimitiveRecords {
    PrimitiveRecord primitive_records[];
};

struct InstanceRecord {
    mat4 transform;
    mat4 inverse_transform;
    uvec4 metadata;
};
layout(set = 0, binding = 22, std430) readonly buffer SceneInstanceRecords {
    InstanceRecord instance_records[];
};

struct LightRecord {
    uvec4 metadata;
    vec4 data;
};
layout(set = 0, binding = 24, std430) readonly buffer SceneLightRecords {
    LightRecord light_records[];
};

struct MeshRecord {
    uvec4 vertex_index_data;
    uvec4 primitive_data;
    uvec4 bvh_data;
    uvec4 flags;
};
layout(set = 0, binding = 25, std430) readonly buffer SceneMeshRecords {
    MeshRecord mesh_records[];
};

struct LocalVertex {
    vec4 position_uv_x;
    vec4 normal_uv_y;
    vec4 tangent;
};
layout(set = 0, binding = 26, std430) readonly buffer LocalMeshVertices {
    LocalVertex local_mesh_vertices[];
};
layout(set = 0, binding = 27, std430) readonly buffer LocalMeshIndices {
    uint local_mesh_indices[];
};
layout(set = 0, binding = 34, std430) readonly buffer RtTriangleMaterialIds {
    uint rt_triangle_material_ids[];
};

layout(set = 0, binding = 30, std430) readonly buffer LocalTriangleData {
    vec4 local_triangle_data[];
};

struct Material {
    vec3 color;
    float roughness;
    float ior;
    uint mat_type;
    float metallic;
    float pad2;
    vec3 emissive;
    float alpha_factor;
    int base_color_texture;
    int normal_texture;
    int metallic_roughness_texture;
    int emissive_texture;
    float alpha_cutoff;
    uint alpha_mode;
    uint double_sided;
};

struct RayPayload {
    uint hit;
    float t;
    vec3 world_pos;
    uint material_id;
    vec3 geom_normal;
    uint front_face;
    vec3 normal;
    uint instance_id;
    uint mesh_id;
    uint primitive_id;
    vec2 uv;
    vec3 tangent;
    vec3 bitangent;
};

const float PI = 3.14159265358979323846;
const uint TRI_STRIDE = 12u;
const uint MATERIAL_STRIDE = 5u;
const int MATERIAL_TEXTURE_LIMIT = 128;
const uint MATERIAL_FLAG_MANUAL_BASE_COLOR_SRGB = 1u << 0u;
const uint MATERIAL_FLAG_MANUAL_EMISSIVE_SRGB = 1u << 1u;
const uint ALPHA_MODE_OPAQUE = 0u;
const uint ALPHA_MODE_MASK = 1u;
const uint ALPHA_MODE_BLEND = 2u;
const float SHADOW_SELF_HIT_EPSILON = 0.001;
const float DEBUG_WHITE_ENV_RADIANCE = 4.0;

uint pack_unorm2x16(vec2 v) {
    uint x = uint(round(clamp(v.x, 0.0, 1.0) * 65535.0));
    uint y = uint(round(clamp(v.y, 0.0, 1.0) * 65535.0));
    return x | (y << 16u);
}

uint pack_snorm2x16(vec2 v) {
    return pack_unorm2x16(v * 0.5 + vec2(0.5));
}

uint encode_octahedral_normal(vec3 n) {
    float denom = abs(n.x) + abs(n.y) + abs(n.z) + 1e-8;
    vec2 p = n.xy / denom;
    if (n.z < 0.0) {
        p = (vec2(1.0) - abs(p.yx)) * sign(p);
    }
    return pack_snorm2x16(p);
}

uvec2 pack_depth_normal(float depth, vec3 normal) {
    return uvec2(floatBitsToUint(depth), encode_octahedral_normal(normal));
}

uint pack_variance(float v) {
    return pack_unorm2x16(vec2(clamp(v / 64.0, 0.0, 1.0), 0.0));
}

uvec2 pack_world_position(vec3 world_pos) {
    vec3 relative = clamp((world_pos - camera.pos.xyz) / 8.0, vec3(-1.0), vec3(1.0));
    return uvec2(pack_snorm2x16(relative.xy), pack_snorm2x16(vec2(relative.z, 0.0)));
}

uint pcg_hash(uint inputValue) {
    uint state = inputValue * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

vec3 id_color(uint id) {
    uint h = pcg_hash(id + 1u);
    return vec3(float(h & 255u), float((h >> 8u) & 255u), float((h >> 16u) & 255u)) / 255.0;
}

float rand_f32(inout uint state) {
    state = pcg_hash(state);
    return float(state) / float(0xffffffffu);
}

vec3 rand_in_unit_sphere(inout uint state) {
    for (int i = 0; i < 64; ++i) {
        vec3 p = vec3(rand_f32(state), rand_f32(state), rand_f32(state)) * 2.0 - 1.0;
        if (dot(p, p) < 1.0) {
            return p;
        }
    }
    return vec3(0.0, 1.0, 0.0);
}

vec3 rand_unit_vector(inout uint state) {
    return normalize(rand_in_unit_sphere(state));
}

Material decode_material(uint mat_idx) {
    uint idx = min(mat_idx, max(mesh_params.material_count, 1u) - 1u) * MATERIAL_STRIDE;
    vec4 d0 = mesh_materials[idx + 0u];
    vec4 d1 = mesh_materials[idx + 1u];
    vec4 d2 = mesh_materials[idx + 2u];
    vec4 d3 = mesh_materials[idx + 3u];
    vec4 d4 = mesh_materials[idx + 4u];
    Material m;
    m.color = d0.xyz;
    m.roughness = d0.w;
    m.ior = d1.x;
    m.mat_type = uint(d1.y);
    m.metallic = d1.z;
    m.pad2 = d1.w;
    m.emissive = d2.xyz;
    m.alpha_factor = d2.w;
    m.base_color_texture = int(round(d3.x));
    m.normal_texture = int(round(d3.y));
    m.metallic_roughness_texture = int(round(d3.z));
    m.emissive_texture = int(round(d3.w));
    m.alpha_cutoff = d4.x;
    m.alpha_mode = uint(round(d4.y));
    m.double_sided = uint(round(d4.z));
    return m;
}

uint material_for_raw_triangle(uint meshFirstIndex, uint primitiveId) {
    uint triangleIndex = meshFirstIndex / 3u + primitiveId;
    uint triangleCount = mesh_params.local_index_count / 3u;
    if (triangleIndex >= triangleCount) {
        return 0u;
    }
    return rt_triangle_material_ids[triangleIndex];
}

void apply_material_textures(inout Material material, vec2 uv) {
    uint flags = uint(round(material.pad2));
    if (material.base_color_texture >= 0 && material.base_color_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.base_color_texture;
        vec4 base = texture(sampler2D(material_textures[nonuniformEXT(textureIndex)], material_samplers[nonuniformEXT(textureIndex)]), uv);
        vec3 baseColor = (flags & MATERIAL_FLAG_MANUAL_BASE_COLOR_SRGB) != 0u
            ? pow(max(base.rgb, vec3(0.0)), vec3(2.2))
            : base.rgb;
        material.color *= baseColor;
        material.alpha_factor *= base.a;
    }
    if (material.metallic_roughness_texture >= 0 && material.metallic_roughness_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.metallic_roughness_texture;
        vec4 mr = texture(sampler2D(material_textures[nonuniformEXT(textureIndex)], material_samplers[nonuniformEXT(textureIndex)]), uv);
        material.roughness = clamp(material.roughness * mr.g, 0.02, 1.0);
        material.metallic = clamp(material.metallic * mr.b, 0.0, 1.0);
        if (material.mat_type == 0u || material.mat_type == 3u) {
            material.mat_type = material.metallic > 0.5 ? 3u : 0u;
        }
    }
    if (material.emissive_texture >= 0 && material.emissive_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.emissive_texture;
        vec4 emissive = texture(sampler2D(material_textures[nonuniformEXT(textureIndex)], material_samplers[nonuniformEXT(textureIndex)]), uv);
        vec3 emissiveColor = (flags & MATERIAL_FLAG_MANUAL_EMISSIVE_SRGB) != 0u
            ? pow(max(emissive.rgb, vec3(0.0)), vec3(2.2))
            : emissive.rgb;
        material.emissive *= emissiveColor;
    }
}

void apply_material_alpha_texture(inout Material material, vec2 uv) {
    if (material.base_color_texture >= 0 && material.base_color_texture < MATERIAL_TEXTURE_LIMIT) {
        int textureIndex = material.base_color_texture;
        vec4 base = texture(sampler2D(material_textures[nonuniformEXT(textureIndex)], material_samplers[nonuniformEXT(textureIndex)]), uv);
        material.alpha_factor *= base.a;
    }
}

bool accept_material_alpha(Material material, uint stableSeed) {
    if (material.alpha_mode == ALPHA_MODE_MASK) {
        return material.alpha_factor >= material.alpha_cutoff;
    }
    if (material.alpha_mode == ALPHA_MODE_BLEND) {
        float threshold = float(pcg_hash(stableSeed)) / float(0xffffffffu);
        return threshold <= clamp(material.alpha_factor, 0.0, 1.0);
    }
    return true;
}

vec3 apply_normal_texture(Material material, vec2 uv, vec3 normal, vec3 tangent, vec3 bitangent, vec3 rayDirection) {
    if (material.normal_texture < 0 || material.normal_texture >= MATERIAL_TEXTURE_LIMIT) {
        return normal;
    }
    int textureIndex = material.normal_texture;
    vec3 tangentSample = texture(sampler2D(material_textures[nonuniformEXT(textureIndex)], material_samplers[nonuniformEXT(textureIndex)]), uv).xyz * 2.0 - 1.0;
    vec3 t = normalize(tangent - normal * dot(normal, tangent));
    vec3 b = normalize(bitangent - normal * dot(normal, bitangent));
    vec3 mapped = normalize(t * tangentSample.x + b * tangentSample.y + normal * max(tangentSample.z, 0.0));
    return dot(mapped, rayDirection) > 0.0 ? -mapped : mapped;
}

Material apply_debug_material_mode(Material material) {
    if (debug_params.view == 22u || debug_params.view == 27u) {
        material.color = vec3(0.72, 0.70, 0.66);
        material.roughness = 0.85;
        material.metallic = 0.0;
        material.mat_type = 0u;
        material.emissive = vec3(0.0);
    }
    return material;
}

vec3 rotate_y(vec3 v, float angle);
vec2 env_uv_from_dir(vec3 dir);

vec3 environment_color(vec3 direction) {
    if (debug_params.view == 27u) {
        return vec3(DEBUG_WHITE_ENV_RADIANCE);
    }
    if (env_params.enabled != 0u && env_params.width > 1u && env_params.height > 1u) {
        vec3 localDir = rotate_y(direction, env_params.rotation);
        vec2 uv = env_uv_from_dir(localDir);
        float scale = env_params.procedural != 0u ? camera.sky_intensity : env_params.intensity;
        return texture(sampler2D(env_map, env_sampler), vec2(fract(uv.x), clamp(uv.y, 0.0, 1.0))).rgb *
            scale * env_params.background_intensity;
    }
    float t = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    return mix(vec3(0.70, 0.74, 0.80), vec3(0.56, 0.68, 0.92), t) *
        camera.sky_intensity * env_params.background_intensity;
}

vec3 rotate_y(vec3 v, float angle) {
    float c = cos(angle);
    float s = sin(angle);
    return vec3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

vec2 env_uv_from_dir(vec3 dir) {
    vec3 d = normalize(dir);
    return vec2(atan(d.z, d.x) / (2.0 * PI) + 0.5, asin(clamp(d.y, -1.0, 1.0)) / PI + 0.5);
}

vec3 env_dir_from_uv(vec2 uv) {
    float phi = (uv.x - 0.5) * 2.0 * PI;
    float lat = (uv.y - 0.5) * PI;
    float cosLat = cos(lat);
    return normalize(vec3(cosLat * cos(phi), sin(lat), cosLat * sin(phi)));
}

vec3 analytical_sun_direction() {
    return rotate_y(normalize(vec3(-0.55057, 0.82812, -0.10531)), -env_params.rotation);
}

vec3 analytical_sun_center_radiance() {
    if (camera.sunlight_enabled != 0u) {
        float sunBrightness = camera.sun_intensity * 16.0;
        return vec3(sunBrightness, sunBrightness * 0.95, sunBrightness * 0.8);
    }
    return vec3(0.0);
}

vec3 analytical_sun_radiance(vec3 dir) {
    if (camera.sunlight_enabled == 0u) {
        return vec3(0.0);
    }
    float cosAngle = dot(normalize(dir), analytical_sun_direction());
    if (cosAngle <= cos(camera.sun_angular_radius)) {
        return vec3(0.0);
    }
    return analytical_sun_center_radiance();
}

vec3 environment_radiance(vec3 dir) {
    if (debug_params.view == 27u) {
        return vec3(DEBUG_WHITE_ENV_RADIANCE);
    }
    if (env_params.enabled != 0u) {
        vec3 localDir = rotate_y(dir, env_params.rotation);
        vec2 uv = env_uv_from_dir(localDir);
        vec3 sampled = texture(sampler2D(env_map, env_sampler), vec2(fract(uv.x), clamp(uv.y, 0.0, 1.0))).rgb;
        float scale = env_params.procedural != 0u ? camera.sky_intensity : env_params.intensity;
        return sampled * scale + analytical_sun_radiance(dir);
    }
    float t = 0.5 * (dir.y + 1.0);
    return mix(vec3(0.70, 0.74, 0.80), vec3(0.56, 0.68, 0.92), t) * camera.sky_intensity + analytical_sun_radiance(dir);
}

vec3 debug_display_tonemap(vec3 color) {
    color = max(color, vec3(0.0));
    color = color / (color + vec3(1.0));
    return pow(color, vec3(1.0 / 2.2));
}

float environment_pdf(vec3 dir) {
    if (env_params.enabled == 0u || env_params.width == 0u || env_params.height == 0u || env_params.inv_total_lum <= 0.0) {
        return 0.0;
    }
    vec3 localDir = rotate_y(dir, env_params.rotation);
    vec2 uv = env_uv_from_dir(localDir);
    uint col = uint(clamp(uv.x * float(env_params.width), 0.0, float(env_params.width - 1u)));
    uint row = uint(clamp(uv.y * float(env_params.height), 0.0, float(env_params.height - 1u)));
    vec3 sampleValue = texelFetch(sampler2D(env_map, env_sampler), ivec2(int(col), int(row)), 0).rgb;
    float lum = dot(sampleValue, vec3(0.2126, 0.7152, 0.0722));
    return max(lum * float(env_params.width) * float(env_params.height) * env_params.inv_total_lum / (2.0 * PI * PI), 0.0);
}

vec3 sample_environment_direction(inout uint state, out vec3 out_dir, out float out_pdf) {
    out_pdf = 0.0;
    if (env_params.enabled == 0u || env_params.width == 0u || env_params.height == 0u) {
        out_dir = vec3(0.0, 1.0, 0.0);
        return vec3(0.0);
    }

    float xi0 = rand_f32(state);
    uint rowLo = 0u;
    uint rowHi = env_params.height - 1u;
    for (uint guard = 0u; guard < 32u && rowLo < rowHi; ++guard) {
        uint mid = (rowLo + rowHi) / 2u;
        if (env_cdf_rows[mid] < xi0) {
            rowLo = mid + 1u;
        } else {
            rowHi = mid;
        }
    }
    uint row = rowLo;

    float xi1 = rand_f32(state);
    uint colLo = 0u;
    uint colHi = env_params.width - 1u;
    uint colOffset = row * env_params.width;
    for (uint guard = 0u; guard < 32u && colLo < colHi; ++guard) {
        uint mid = (colLo + colHi) / 2u;
        if (env_cdf_cols[colOffset + mid] < xi1) {
            colLo = mid + 1u;
        } else {
            colHi = mid;
        }
    }
    uint col = colLo;
    vec2 uv = vec2((float(col) + 0.5) / float(env_params.width), (float(row) + 0.5) / float(env_params.height));
    out_dir = rotate_y(env_dir_from_uv(uv), -env_params.rotation);
    float scale = env_params.procedural != 0u ? camera.sky_intensity : env_params.intensity;
    vec3 radiance = texelFetch(sampler2D(env_map, env_sampler), ivec2(int(col), int(row)), 0).rgb * scale;
    out_pdf = environment_pdf(out_dir);
    return radiance;
}

float power_heuristic(float pdf_a, float pdf_b) {
    float a2 = pdf_a * pdf_a;
    float b2 = pdf_b * pdf_b;
    return a2 / max(a2 + b2, 1e-8);
}

float reflectance(float cosine, float ref_idx) {
    float r0 = (1.0 - ref_idx) / (1.0 + ref_idx);
    r0 = r0 * r0;
    return r0 + (1.0 - r0) * pow(clamp(1.0 - cosine, 0.0, 1.0), 5.0);
}

float diffuse_pdf(vec3 normal, vec3 wi) {
    return max(dot(normal, wi), 0.0) / PI;
}

vec3 sample_cosine_hemisphere(inout uint state, vec3 normal, out float pdf) {
    float r1 = 2.0 * PI * rand_f32(state);
    float r2 = rand_f32(state);
    float r2s = sqrt(r2);
    vec3 axis = abs(normal.x) > 0.1 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(axis, normal));
    vec3 bitangent = cross(normal, tangent);
    vec3 dir = normalize(tangent * cos(r1) * r2s + bitangent * sin(r1) * r2s + normal * sqrt(max(1.0 - r2, 0.0)));
    pdf = diffuse_pdf(normal, dir);
    return dir;
}

float ggx_ndf(float roughness, float n_dot_h) {
    float r = max(roughness, 0.02);
    float a = r * r;
    float a2 = a * a;
    float d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-10);
}

vec3 schlick_fresnel(vec3 f0, float v_dot_h) {
    float f = pow(clamp(1.0 - v_dot_h, 0.0, 1.0), 5.0);
    return f0 + (vec3(1.0) - f0) * f;
}

float smith_g1(float roughness, float n_dot_x) {
    float r = max(roughness, 0.02);
    float a = r * r;
    float a2 = a * a;
    float n2 = n_dot_x * n_dot_x;
    return 2.0 * n_dot_x / max(n_dot_x + sqrt(a2 + (1.0 - a2) * n2), 1e-10);
}

float smith_g(float roughness, float n_dot_v, float n_dot_l) {
    return smith_g1(roughness, n_dot_v) * smith_g1(roughness, n_dot_l);
}

vec3 eval_ggx_brdf(Material material, vec3 wo, vec3 wi, vec3 n) {
    float n_dot_v = max(dot(n, wo), 0.0);
    float n_dot_l = max(dot(n, wi), 0.0);
    if (n_dot_v < 1e-6 || n_dot_l < 1e-6) {
        return vec3(0.0);
    }
    vec3 h = normalize(wo + wi);
    float n_dot_h = max(dot(n, h), 0.0);
    float v_dot_h = max(dot(wo, h), 0.0);
    vec3 f0 = mix(vec3(0.04), material.color, clamp(material.metallic, 0.0, 1.0));
    vec3 f = schlick_fresnel(f0, v_dot_h);
    float d = ggx_ndf(material.roughness, n_dot_h);
    float g = smith_g(material.roughness, n_dot_v, n_dot_l);
    vec3 specular = f * d * g / max(4.0 * n_dot_v * n_dot_l, 1e-10);
    vec3 kd = mix(vec3(1.0) - f, vec3(0.0), clamp(material.metallic, 0.0, 1.0));
    vec3 diffuse = kd * material.color * (1.0 / PI);
    return diffuse + specular;
}

float pdf_ggx_brdf(Material material, vec3 wo, vec3 wi, vec3 n) {
    float n_dot_v = max(dot(n, wo), 0.0);
    float n_dot_l = max(dot(n, wi), 0.0);
    if (n_dot_v < 1e-6 || n_dot_l < 1e-6) {
        return 0.0;
    }
    vec3 h = normalize(wo + wi);
    float n_dot_h = max(dot(n, h), 0.0);
    float v_dot_h = max(dot(wo, h), 0.0);
    float d = ggx_ndf(material.roughness, n_dot_h);
    return d * n_dot_h / max(4.0 * v_dot_h, 1e-10);
}

vec3 sample_ggx_brdf(inout uint state, Material material, vec3 wo, vec3 n) {
    float r = max(material.roughness, 0.02);
    float a = r * r;
    float r1 = rand_f32(state);
    float r2 = rand_f32(state);
    float phi = r1 * 2.0 * PI;
    float cosTheta = sqrt((1.0 - r2) / max(1.0 + (a * a - 1.0) * r2, 1e-8));
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    vec3 axis = abs(n.x) > 0.1 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(axis, n));
    vec3 bitangent = cross(n, tangent);
    vec3 h = normalize(tangent * cos(phi) * sinTheta + bitangent * sin(phi) * sinTheta + n * cosTheta);
    return normalize(2.0 * dot(wo, h) * h - wo);
}

vec3 eval_brdf(Material material, vec3 wo, vec3 wi, vec3 n) {
    if (material.mat_type == 0u) {
        return material.color * (1.0 / PI);
    }
    if (material.mat_type == 3u || material.mat_type == 4u) {
        return eval_ggx_brdf(material, wo, wi, n);
    }
    return vec3(0.0);
}

float pdf_brdf(Material material, vec3 wo, vec3 wi, vec3 n) {
    if (material.mat_type == 0u) {
        return diffuse_pdf(n, wi);
    }
    if (material.mat_type == 3u || material.mat_type == 4u) {
        return pdf_ggx_brdf(material, wo, wi, n);
    }
    return 0.0;
}

vec3 sample_brdf(inout uint state, Material material, vec3 wo, vec3 n, out float pdf) {
    if (material.mat_type == 3u || material.mat_type == 4u) {
        vec3 wi = sample_ggx_brdf(state, material, wo, n);
        pdf = pdf_ggx_brdf(material, wo, wi, n);
        return wi;
    }
    return sample_cosine_hemisphere(state, n, pdf);
}
