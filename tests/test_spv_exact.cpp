// spv_exact::scan (gpu/vk/spv_exact.h): the texture bindings a shader reads by texel position or asks the size of.
// tests/test_spv_exact.py
#include "../gpu/vk/spv_exact.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static int failures = 0;
static void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

struct Module {
    std::vector<uint32_t> w{0x07230203u, 0x00010500u, 0, 100, 0};
    void op(uint16_t code, std::initializer_list<uint32_t> args) {
        w.push_back((uint32_t)(args.size() + 1) << 16 | code);
        for (uint32_t a : args) w.push_back(a);
    }
    // a combined image sampler variable `var` at `binding`: dim 1 is 2D, 2 is 3D
    void texture(uint32_t var, uint32_t binding, uint32_t dim, bool arrayed, bool ms = false, uint32_t sampled = 1, bool in_array = false) {
        const uint32_t base = var * 10;
        op(71, {var, 33, binding});                                  // OpDecorate Binding
        op(71, {var, 34, 0});                                        // OpDecorate DescriptorSet
        op(25, {base + 1, 1, dim, 0, arrayed ? 1u : 0u, ms ? 1u : 0u, sampled, 0});   // OpTypeImage
        op(27, {base + 2, base + 1});                                // OpTypeSampledImage
        uint32_t t = base + 2;
        if (in_array) op(28, {base + 3, t, 2}), t = base + 3;       // OpTypeArray
        op(32, {base + 4, 0, t});                                    // OpTypePointer
        op(59, {base + 4, var, 0});                                  // OpVariable
    }
    // the image of the variable, loaded: returns its id
    uint32_t image(uint32_t var, uint32_t id, bool through_chain = false) {
        uint32_t from = var;
        if (through_chain) op(65, {0, id + 2, var, 7}), from = id + 2;   // OpAccessChain
        op(61, {0, id, from});                                           // OpLoad
        op(100, {0, id + 1, id});                                        // OpImage
        return id + 1;
    }
    spv_exact::Bindings scan() const { return spv_exact::scan(w.data(), w.size()); }
};

int main() {
    using spv_exact::has;
    using spv_exact::any;
    {   // sampled only: nothing
        Module m;
        m.texture(10, 3, 1, false);
        m.op(61, {0, 200, 10});                 // OpLoad
        m.op(87, {0, 201, 200, 202});           // OpImageSampleImplicitLod
        expect(!any(m.scan()), "a sampled read marks nothing");
    }
    {   // a fetch
        Module m;
        m.texture(10, 3, 1, false);
        m.texture(11, 4, 1, false);
        m.op(95, {0, 300, m.image(10, 200), 301});   // OpImageFetch
        const auto b = m.scan();
        expect(has(b, 3) && !has(b, 4), "a fetch marks its binding and no other");
    }
    {   // a fetch through an access chain into an array of textures
        Module m;
        m.texture(10, 9, 1, false, false, 1, true);
        m.op(95, {0, 300, m.image(10, 200, true), 301});
        expect(has(m.scan(), 9), "a fetch through an array of textures marks the array's binding");
    }
    {   // the size of a 2D texture
        Module m;
        m.texture(10, 5, 1, false);
        m.op(103, {0, 300, m.image(10, 200), 301});   // OpImageQuerySizeLod
        m.op(81, {0, 302, 300, 0});                   // .x
        expect(has(m.scan(), 5), "a size query on a 2D texture marks it");
    }
    {   // the number of levels
        Module m;
        m.texture(10, 6, 1, true);
        m.op(106, {0, 300, m.image(10, 200)});        // OpImageQueryLevels
        expect(has(m.scan(), 6), "a level count query marks it");
    }
    {   // the layer count of an array alone
        Module m;
        m.texture(10, 7, 1, true);
        m.op(103, {0, 300, m.image(10, 200), 301});
        m.op(81, {0, 302, 300, 2});                   // .z
        m.op(81, {0, 303, 300, 2});
        expect(!any(m.scan()), "an array's size used for its layer count alone marks nothing");
    }
    {   // an array's size used for its width too
        Module m;
        m.texture(10, 7, 1, true);
        m.op(103, {0, 300, m.image(10, 200), 301});
        m.op(81, {0, 302, 300, 2});
        m.op(81, {0, 303, 300, 0});
        expect(has(m.scan(), 7), "an array's size used for its width marks it");
    }
    {   // an array's size handed on whole
        Module m;
        m.texture(10, 7, 1, true);
        m.op(103, {0, 300, m.image(10, 200), 301});
        m.op(111, {0, 302, 300});                     // OpConvertSToF of the whole vector
        expect(has(m.scan(), 7), "an array's size used whole marks it");
    }
    {   // a 2D texture's third component does not exist: any use of its size counts
        Module m;
        m.texture(10, 8, 1, false);
        m.op(103, {0, 300, m.image(10, 200), 301});
        expect(has(m.scan(), 8), "a 2D texture's size query marks it whatever is done with it");
    }
    {   // the kinds the texture detail never touches
        Module m;
        m.texture(10, 1, 2, false);                   // 3D
        m.texture(11, 2, 1, false, true);             // multisampled
        m.texture(12, 3, 1, false, false, 2);         // a storage image
        m.op(95, {0, 300, m.image(10, 200), 301});
        m.op(95, {0, 310, m.image(11, 210), 311});
        m.op(104, {0, 320, m.image(12, 220)});        // OpImageQuerySize
        expect(!any(m.scan()), "3D, multisampled and storage images mark nothing");
    }
    {   // two textures, one fetched and one sampled, bindings past 63
        Module m;
        m.texture(10, 70, 1, false);
        m.texture(11, 200, 1, true);
        m.op(95, {0, 300, m.image(11, 200), 301});
        const auto b = m.scan();
        expect(has(b, 200) && !has(b, 70) && !has(b, 255) && !has(b, 300), "bindings up to 255");
    }
    {   // not SPIR-V, or cut short
        const uint32_t junk[] = {1, 2, 3, 4, 5, 6};
        expect(!any(spv_exact::scan(junk, 6)), "another magic number");
        Module m;
        m.texture(10, 3, 1, false);
        m.op(95, {0, 300, m.image(10, 200), 301});
        m.w.push_back(9u << 16 | 95);                 // an instruction longer than what is left
        expect(has(m.scan(), 3), "a module cut short keeps what was read");
        expect(!any(spv_exact::scan(m.w.data(), 3)), "shorter than a header");
    }
    if (failures) return EXIT_FAILURE;
    std::printf("spv exact: all checks passed\n");
    return EXIT_SUCCESS;
}
