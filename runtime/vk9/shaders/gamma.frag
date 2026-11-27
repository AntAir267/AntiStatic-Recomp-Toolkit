// Our own Vulkan renderer: the gamma ramp's pass at present (runtime/vk_d3d9.c "gamma"). Direct3D's
// SetGammaRamp as DXVK 3.1.1's presenter applies it (src/dxvk/shaders/dxvk_present_frag.frag and
// dxvk_present_common.glsl as DxvkSwapchainBlitter specializes them for a D3D9 back buffer and an sRGB non-linear
// swapchain: both colour spaces the same, so "pass-through"; c_gamma on; the back buffer read through a UNORM
// view, c_src_is_srgb false). Each channel is looked up in the ramp: a 256-texel R16G16B16A16 image filtered
// linearly with clamped edges, at the channel's value itself; here a 256x1 2D texture of the bindless set sampled
// at its row's centre, which filters as the 1D one along u. DXVK's presenter makes the swapchain's images
// mutable and writes through an sRGB view when the device allows (VK_KHR_swapchain_mutable_format,
// dxvk_presenter.cpp), so c_dst_is_srgb is true: the looked-up value taken to linear here and back to sRGB by the
// image's store. vk_core.c does the same (R.sc_views_srgb), and slots[5] says so; without it, the value as it is
// (DXVK's too, on a device without the extension). Round 3: without the sRGB view one level off DXVK on 70-98 %
// of a flash's pixels [verified: the pass's frame dumps against DXVK's, lavapipe].
// Push constants: slots[0] the back buffer (texture | sampler << 16), slots[1] the ramp, slots[2] 1 when
// the back buffer is the target's size (texel fetches), slots[3-4] the target's size, slots[5] 1: the target is
// an sRGB view.
#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform texture2D tex2D[];
layout(set = 0, binding = 4) uniform sampler samplers[];
layout(push_constant) uniform PC { uint slots[8]; } pc;
layout(location = 0) out vec4 o_color;

#define BB sampler2D(tex2D[pc.slots[0] & 0xffffu], samplers[pc.slots[0] >> 16])
#define LUT sampler2D(tex2D[pc.slots[1] & 0xffffu], samplers[pc.slots[1] >> 16])

// (dxvk_color_space.glsl's, as written there)
vec3 srgb_to_linear(vec3 srgb) {
  bvec3 isLo = lessThanEqual(srgb, vec3(0.04045f));

  vec3 loPart = srgb / 12.92f;
  vec3 hiPart = pow((srgb + 0.055f) / 1.055f, vec3(12.0f / 5.0f));
  return mix(hiPart, loPart, isLo);
}

void main() {
    vec4 color;
    if (pc.slots[2] != 0u)
        color = texelFetch(BB, ivec2(gl_FragCoord.xy), 0);
    else
        color = texture(BB, gl_FragCoord.xy / vec2(float(pc.slots[3]), float(pc.slots[4])));
    color.rgb = vec3(texture(LUT, vec2(color.r, 0.5)).r, texture(LUT, vec2(color.g, 0.5)).g, texture(LUT, vec2(color.b, 0.5)).b);
    if (pc.slots[5] != 0u)
        color.rgb = srgb_to_linear(color.rgb);
    o_color = color;
}
