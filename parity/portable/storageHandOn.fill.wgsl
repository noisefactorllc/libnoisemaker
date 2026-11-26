@group(0) @binding(0) var outputBuffer: texture_storage_2d<rgba16float, write>;
@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (any(gid.xy >= textureDimensions(outputBuffer))) { return; }
    let color = vec4<f32>(f32((gid.x / 8u) % 4u) * 0.25, f32((gid.y / 8u) % 4u) * 0.25, 0.5, 1.0);
    textureStore(outputBuffer, vec2<i32>(gid.xy), color);
}
