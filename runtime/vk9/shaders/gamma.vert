// Our own Vulkan renderer: the gamma ramp's pass at present (runtime/vk_d3d9.c "gamma"): one triangle
// over the whole swapchain image.
#version 450

void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
