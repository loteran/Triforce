/* (Triforce) The Insert menu's Anti-aliasing row (vk_menu.cpp), and the rule that finds the game's own anti-aliasing passes
   among a frame's compute dispatches (vk_draw.cpp aa_stand_in, vk_aa.cpp), as plain functions. No Vulkan, no menu:
   tests/test_aa_row.py compiles and runs them. After NX2navi's aa setting (its runtime/vk_aa.c and vk_exec_draw.c
   aa_dispatch).

   OoT anti-aliases every frame of the 3D with SMAA in three compute passes, right after the draw that adds the bloom and
   tone maps: the edges (4 x 4 pixel tiles), the blending weights from them (an indirect dispatch over the tiles with edges)
   and the composition, which samples the tone mapped picture and the weights and stores the smoothed picture into the
   target the gameplay upscale then takes. It stores each channel's square root, the three packed as one RGB9E5 word, through
   an integer view of that B10G11R11 target (shaders/aa_common.glsl). */
#pragma once

#include <cstdint>

#include "upscaler_rows.h"

namespace mkwvk::aa_rows {

/// The Anti-aliasing row (opts.aa, saved as anti-aliasing; RX_VK_AA over it): the game's own SMAA as it is, none (the tone
/// mapped picture stored as it is), FXAA over it (shaders/aa_fxaa.glsl), or SMAA at its Ultra preset in place of the game's
/// (shaders/smaa_*.comp). The saved values keep their meaning: a new choice comes after these
enum { GAME = 0, OFF = 1, FXAA = 2, SMAA = 3, kValues = 4 };
inline const char* name(int v) {
    switch (v) {
    case OFF: return "Off";
    case FXAA: return "FXAA";
    case SMAA: return "SMAA Ultra";
    default: return "Game SMAA";   // (also for a value past the row; "Game", the owner's word, 11-16)
    }
}
/// The row's value as one of its choices, RX_VK_AA=<0..3> over it (env: its number, -1 unset). Anything else is the game's own
inline int chosen(int row, int env) {
    const int v = env >= 0 ? env : row;
    return v >= GAME && v < kValues ? v : GAME;
}
/// DLSS, DLAA, FSR 3.1, FSR 4 and their native modes anti-alias the 3D themselves, from frames the game's own pass has not
/// smoothed any better: while one of them draws (upscaler: what runs, upscaler_rows.h) the row does not apply
inline bool upscaler_own(int upscaler) { return upscaler != upscaler_rows::GAME; }
/// What is in effect: the choice, or the game's own while such an upscaler draws (its frame is then drawn as before the row)
inline int effective(int choice, int upscaler) { return upscaler_own(upscaler) ? (int)GAME : choice; }
/// The row has nothing to choose then (shown without arrows; Left and Right leave the saved value alone), nor with
/// RX_VK_AA set for the run
inline bool fixed(int upscaler, bool env_set) { return upscaler_own(upscaler) || env_set; }

// ---- the game's passes ----

/// The game's three SMAA programs, by their keys (vk_shader.cpp code_key: a hash of the compute stage's control section and
/// code as the game hands them over, the same on every PC and with every setting). Game build e0f3ccfa. NX2navi knows the
/// same three by another hash of the same code (fcb21f2afa88378c, e44377cbfafc1101, 470d661afb1cb73a in its vk_aa.c): these
/// are the programs of a build's shader list (triforce_shaders.bin) with those hashes. Another game version has other keys:
/// nothing is then found, and the frame is drawn as before the row (vk_aa.cpp says so in the log)
enum { NONE = 0, EDGES = 1, WEIGHTS = 2, COMPOSE = 3 };
constexpr uint64_t kEdgesKey = 0x26fe0c70ab9fc547ull, kWeightsKey = 0x685adb41edc19de7ull, kComposeKey = 0x6637ac1d052d22d4ull;
constexpr int role(uint64_t key) {
    return key == kEdgesKey ? EDGES : key == kWeightsKey ? WEIGHTS : key == kComposeKey ? COMPOSE : NONE;
}

/// What a dispatch has at one of its program's texture or image bindings, as far as the rule looks
enum { OTHER = 0, R11 = 1, E5 = 2 };   // (the image's own format: B10G11R11, the scene's and the picture's; E5B9G9R9; anything else)
struct Bound {
    uint64_t image = 0;        // (which image: any number that tells two apart; 0 nothing bound)
    int format = OTHER;
    uint32_t width = 0, height = 0;
    bool flat = false;         // (a plain 2D image, one sample, seen as 2D at its first level)
    bool written = false;      // (an image binding the program stores into)
    bool integer = false;      // (the program reads or stores it as integer words, not as colours: the variant of the program
                               // that runs is the one translated for the views bound, so this is the view's kind)
};
/// The composition's two ends among what its dispatch has bound (the program's own bindings, in its order): the target, the one
/// flat B10G11R11 (or E5B9G9R9: the same words) image it stores integer words into, and the tone mapped picture, the one flat
/// B10G11R11 texture of the target's size it samples as colours that is another image (the weights it samples too are read
/// as integer words). src and dst: their places in the lists. why: nullptr when both are found, else what was not as expected;
/// the game's own pass then runs, nothing is guessed
struct Ends {
    int src = -1, dst = -1;
    const char* why = nullptr;
};
inline Ends compose_ends(const Bound* textures, int ntex, const Bound* images, int nimg) {
    Ends e;
    int found = 0;
    for (int i = 0; i < nimg; i++) {
        const Bound& b = images[i];
        if (!b.image || !b.written || !b.integer || !b.flat || (b.format != R11 && b.format != E5) || !b.width || !b.height) continue;
        if (!found++) e.dst = i;
    }
    if (found != 1) return e.dst = -1, e.why = found ? "it stores into more than one such target" : "it stores into no B10G11R11 target as integer words", e;
    const Bound& d = images[e.dst];
    found = 0;
    for (int i = 0; i < ntex; i++) {
        const Bound& b = textures[i];
        if (!b.image || b.image == d.image || b.integer || !b.flat || b.format != R11 || b.width != d.width || b.height != d.height) continue;
        if (!found++) e.src = i;
    }
    if (found != 1) return e.src = e.dst = -1, e.why = found ? "it samples more than one picture of its target's size" : "it samples no B10G11R11 picture of its target's size", e;
    return e;
}

/// Whether a dispatch of the game's edge or weight pass is left out: only the composition reads what they make, so they go
/// while another choice than the game's own is in effect and the composition before this one was replaced. One that could
/// not be replaced runs as the game's, and these two run again from the next frame on, until one is replaced again.
/// keep (RX_VK_AA_KEEP=1): never left out
constexpr bool leave_out(int role, int mode, bool last_replaced, bool keep) {
    return (role == EDGES || role == WEIGHTS) && mode != GAME && last_replaced && !keep;
}

/// What the row says under its help about the passes (vk_aa.cpp aa_state): nothing asked or nothing seen yet, the game's
/// composition replaced, or none replaced for a while with another choice than the game's own in effect
enum { QUIET = 0, REPLACED = 1, MISSING = 2 };
/// The frames with a choice in effect and no composition replaced after which that is said (a minute at 60 a second: a
/// loading screen or a menu without the 3D that lasts longer says it too)
constexpr uint64_t kMissingFrames = 3600;

/// What vk_aa.cpp keeps about the game's passes from frame to frame (one for the run, under its lock)
struct Track {
    int mode = GAME;              // (the choice in effect in the frame before)
    bool last_replaced = false;   // (the last composition met was replaced: what leave_out asks)
    int state = QUIET;
    uint64_t since = 0;           // (the frame the choice came into effect, or a composition was last replaced)
    /// A frame begins with `want` in effect. true: the row's line became MISSING just now (said once in the log)
    constexpr bool begin(uint64_t frame, int want) {
        bool missing = false;
        if (want == GAME) {
            // (the game's own passes all run from here on: a composition replaced before says nothing about the next choice's
            // first frame, whose edge and weight passes must run until one is replaced again)
            last_replaced = false, state = QUIET;
        } else if (mode != want) {
            since = frame, state = QUIET;   // (another choice: counted anew)
        } else if (state != MISSING && frame - since >= kMissingFrames) {
            state = MISSING, missing = true;
        }
        mode = want;
        return missing;
    }
    /// A composition met with a choice in effect: replaced, or left to the game
    constexpr void composed(uint64_t frame, bool replaced) {
        last_replaced = replaced;
        if (replaced) since = frame, state = REPLACED;
    }
};

} // namespace mkwvk::aa_rows
