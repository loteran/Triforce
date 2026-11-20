// (Triforce) Shared by the passes that stand in for the game's SMAA composition (gpu/vk/vk_aa.cpp; aa_post.comp, smaa_luma.comp,
// smaa_blend.comp). After NX2navi's shaders/aa_common.glsl.
//
// The game's composition stores the post-processed picture into its B10G11R11 target as raw words: each channel is the square
// root of its linear value, the three packed together as RGB9E5 (red in bits 0-8, green 9-17, blue 18-26, the shared exponent
// in 27-31: Vulkan's E5B9G9R9 layout), which the gameplay upscale then samples through an RGB9E5 view. The passes here write the
// same words through an R32_UINT view of that target.
//
// Written in what GLSL and C++ share (scalar steps, no swizzles, no arrays): tests/test_aa_shaders.py compiles this file as
// C++ and runs it on authored pixels.

const float AA_MAX_9E5 = 65408.0;   // ((2^9 - 1) / 2^9 * 2^16: the largest value the format holds)

// (a channel as the format can hold it: not negative, not past its largest; a NaN is 0)
float aa_channel_9e5(float v) { return v > 0.0 ? min(v, AA_MAX_9E5) : 0.0; }

// RGB9E5 of a colour: the shared exponent of the largest channel, each channel rounded to 9 bits of it
uint aa_pack_rgb9e5(vec3 c) {
    float r = aa_channel_9e5(c.r), g = aa_channel_9e5(c.g), b = aa_channel_9e5(c.b);
    float m = max(max(r, g), b);
    float e = max(-16.0, floor(log2(max(m, 1e-30)))) + 16.0;   // (biased by 15, plus one)
    float scale = exp2(e - 24.0);                               // (2^(e - 15 - 9))
    if (floor(m / scale + 0.5) >= 512.0) {   // (the largest channel rounds up to the next power of two)
        e += 1.0;
        scale *= 2.0;
    }
    return uint(floor(r / scale + 0.5)) | (uint(floor(g / scale + 0.5)) << 9) | (uint(floor(b / scale + 0.5)) << 18) |
           (uint(e) << 27);
}

// The stored form of a linear colour: its square root, which is also the space the edges are looked for in
vec3 aa_encode(vec3 c) { return sqrt(max(c, vec3(0.0))); }
