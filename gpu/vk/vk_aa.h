// (Triforce) The Insert menu's Anti-aliasing row at work (vk_aa.cpp): what runs in place of the game's own SMAA. After
// NX2navi's anti-aliasing setting (its runtime/vk_aa.c).
//
// The game smooths the 3D's edges with SMAA in three compute passes after its tone mapping (aa_rows.h says which programs
// and how they are told). The row's Game SMAA, the default, leaves them alone: nothing here records anything, and a
// dispatch costs a comparison of its program's key. Off, FXAA and SMAA Ultra leave the game's edge and weight passes out and
// replace its composition (vk_draw.cpp aa_stand_in) with one of the passes here, which read the picture the game's pass
// samples and write the words it would have written: Off stores the picture as it is, FXAA smooths it first
// (shaders/aa_post.comp), SMAA Ultra runs the SMAA reference implementation at its Ultra preset (shaders/smaa_*.comp, with
// third_party/smaa's source and lookup textures). NX2navi's fifth choice, TAA, was DLSS or FSR at the 3D's own size: here
// that is the Upscaler row's NVIDIA DLAA or AMD FSR Native AA, and the row has no entry for it.
//
// While DLSS, DLAA or FSR draws (the Upscaler row, or its switches) the row does not apply: they anti-alias the 3D themselves,
// and the frame is drawn as before the row.
//
// RX_VK_AA=<0..3>: the choice for the run, over the row (0 the game's, 1 off, 2 FXAA, 3 SMAA Ultra). RX_VK_AA_KEEP=1: the
// game's edge and weight passes always run (their cost stays; a test's switch).
#pragma once

#include <cstdint>

#include "vk_internal.h"

namespace mkwvk {

/// What is in effect for the frame being recorded (aa_rows.h GAME, OFF, FXAA, SMAA): the row's choice or RX_VK_AA, and the
/// game's own while an upscaler with its own anti-aliasing draws. Taken once a frame, so the passes of one frame agree
int aa_mode();
/// RX_VK_AA's number when it names a choice for this run (the row then has nothing to choose), -1 unset
int aa_env_choice();
/// false while the choice is the game's own and nothing of an earlier choice is left to free: the frame is then drawn as
/// before the row, and its dispatches ask nothing more than this
bool aa_asked();
/// Every compute dispatch asks, before anything of it is recorded (key: its program's): true for the game's edge and weight
/// passes while they are left out (aa_rows.h leave_out)
bool aa_leave_out(uint64_t key);
/// The game's composition replaced: mode's pass from src (the picture as the game's pass samples it) into dst (an R32_UINT
/// storage view of its target), w x h, both GENERAL. Recorded outside a pass, after the barrier that orders the dispatch;
/// the caller says the bind point's descriptors are gone. false: nothing recorded (its pipelines could not be made)
bool aa_record(VkCommandBuffer cmd, int mode, VkImageView src, VkImageView dst, uint32_t w, uint32_t h);
/// What became of a composition while a choice was in effect: replaced, or left to the game (why: for the log, the first
/// time and when it changes)
void aa_composed(bool replaced, const char* why);
/// For the row's line (aa_rows.h QUIET, REPLACED, MISSING)
int aa_state();

} // namespace mkwvk
