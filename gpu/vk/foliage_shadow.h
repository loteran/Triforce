// (Triforce) Foliage in the sun's far shadow cascades drawn with one face culled (vk_draw.cpp draw_cull, for dynamic_state;
// RX_VK_FOLIAGE_SHADOW_CULL, off unless set), apart from Vulkan so that tests/test_foliage_shadow.py can run its rule.
//
// The game draws its foliage cards (grass, ferns, leaves) double-sided with mesh programs, into the shadow cascades too: the
// sun's three cascades are the layers of one depth array, 1600 x 1600 in the owner's logs. Seen from the sun the stacked,
// alpha-tested cards cover the same texels many times over: NX2navi counted about 1,000 million fragments in one such draw of
// the far cascade. With the switch, a draw of a mesh program (native, or through the compute fallback) into a cascade from the
// given layer on, that culls neither face, is drawn with its back faces culled (its front ones with an "f"): half the cards'
// fragments there. Cull mode is dynamic state, so no other pipeline is made. A cascade here is Triforce's own test for one,
// as for the cascades' depth bias in dynamic_state: depth only, square, wider than 768 (the characters' own shadow maps, 768
// and less, are never touched).
//
// The behaviour of NX2navi's NX2NAVI_FOLIAGE_SHADOW_CULL (epicmeme546's, on there from layer 1), written anew. It is not the same
// picture: a card that faces away from the sun no longer casts its shadow in those cascades. Where cards stand in crossed pairs
// or clumps others cover the same texels, but a lone card's shadow can go, so it is off here: a switch to compare, at
// RX_VK_FOLIAGE_SHADOW_CULL=1 as NX2navi has it.
//
// Not done: NX2navi's second half, NX2NAVI_FOLIAGE_SHADOW_VRS (one fragment shaded per 2 x 2 pixels in those cascades, so the
// leaves' alpha test runs a quarter as often). It needs VK_KHR_fragment_shading_rate enabled on the device, the shading rate as
// dynamic state in every mesh pipeline and its libraries (a change to every mesh pipeline's creation, which the driver's cache
// then misses) and vkCmdSetFragmentShadingRateKHR, which takes pointers, carried by the recorder (vk_recorder.h, vk_work.h);
// none of that can be checked without the game, and its picture is not the same either (a coarser cut-out of the leaves).
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace mkwvk::foliage_shadow {

constexpr int kNone = 0, kFront = 1, kBack = 2;   // (NVN's faces: NVN_FACE_NONE, _FRONT, _BACK)
constexpr uint32_t kCascadeMin = 769;             // (a cascade is wider than 768: vk_draw.cpp dynamic_state's test)

struct Rule {
    int from = -1;        // (the first cascade layer it applies to; -1: off)
    bool front = false;   // (the front faces culled, not the back ones)
};

/// RX_VK_FOLIAGE_SHADOW_CULL=<layer>[f]: from that layer of the cascades on (the game has three, 0 to 2; 1 is NX2navi's), the back
/// faces culled, the front ones with an "f" after the number. Unset, empty or not starting with a digit: off
inline Rule parse(const char* e) {
    Rule r;
    if (!e || *e < '0' || *e > '9') return r;
    r.from = std::atoi(e);
    r.front = std::strchr(e, 'f') != nullptr;
    return r;
}

/// The face a draw culls: the game's (cull, NVN's face), or under the rule the one it names, for a draw of a mesh program into a
/// cascade (no colour target, a depth target width x height, square and wider than 768, the depth view's first layer at
/// rule.from or past it) that culls neither face
inline int cull(const Rule& rule, int game, bool mesh_program, int colour_targets, bool depth, uint32_t width, uint32_t height,
                uint32_t layer) {
    if (rule.from < 0 || game != kNone || !mesh_program || colour_targets != 0 || !depth) return game;
    if (width != height || width < kCascadeMin || layer < (uint32_t)rule.from) return game;
    return rule.front ? kFront : kBack;
}

} // namespace mkwvk::foliage_shadow
