/* (Triforce, options_version 7; 8 on v0.5.1) The Insert menu's rows that each stand for two saved values (vk_menu.cpp), and
   what an options file from before version 8 becomes (v0.5.1 wrote 7 for its Frame pacing default, with the old rows' keys
   still in it; the 436f9ac-based builds wrote 7 for this, without them: kVersion is 8 so that both are moved, and the second
   has nothing to move). The notes "(Triforce, options_version 7)" across the source and the tests name this change by its
   436f9ac-based number: on this line it is version 8, and version 7 is v0.5.1's Frame pacing default (vk_menu.cpp
   menu_init, "version < 7"); a file of 7 can still have the old rows' keys. The owner: "there's multiple settings that do
   multiple things and they contradict each other ... If a setting can be implemented in some other way, then remove the
   setting entirely". So one row chooses what two did: HDR (and RenoDX HDR), Frame generation (and DLSS frame generation), Frame pacing (and Steady frame rate), Low latency
   (NVIDIA Reflex or AMD Anti-Lag 2, by the graphics card's maker). The saved values keep their keys and their meanings, for the
   renderer and the host read them, some before the menu (hdr=, renodx_hdr=, frame_generation=, dlss_frame_generation=,
   frame_pacing=, steady_frame_rate=, nvidia_reflex=, amd_anti-lag_2=): the rows are views of them.
   Plain functions, no Vulkan, no menu: tests/test_merged_rows.py checks them, with a copy of the owner's own options. */
#pragma once

namespace mkwvk::merged_rows {

// ---- HDR: Off | Expanded | Native | Native + RenoDX (opts.hdr 0 to 2, opts.renodx 0 or 1: RenoDX runs only with Native) ----
enum { HDR_OFF = 0, HDR_EXPANDED = 1, HDR_NATIVE = 2, HDR_RENODX = 3 };
constexpr int hdr_row(int hdr, int renodx) {
    return hdr == HDR_NATIVE && renodx ? (int)HDR_RENODX : hdr < 0 ? 0 : hdr > HDR_NATIVE ? (int)HDR_NATIVE : hdr;
}
inline void hdr_set(int row, int& hdr, int& renodx) {
    hdr = row >= HDR_RENODX ? (int)HDR_NATIVE : row < 0 ? 0 : row;
    renodx = row >= HDR_RENODX;
}

// ---- Frame generation: opts.framegen's own values (framegen_rows.h: 0 Off, 1 Auto, 2 to 8, 9 AMD FSR 4), then NVIDIA DLSS
// 2x, 3x and 4x (opts.dlssg 1 to 3, read at the start: a restart). Choosing one sets the other to Off ----
enum { FG_DLSS = 10, FG_DLSS_LAST = 12 };
constexpr int framegen_row(int framegen, int dlssg) { return dlssg >= 1 && dlssg <= 3 ? FG_DLSS + dlssg - 1 : framegen; }
inline void framegen_set(int row, int& framegen, int& dlssg) {
    if (row >= FG_DLSS) {
        dlssg = row > FG_DLSS_LAST ? 3 : row - FG_DLSS + 1, framegen = 0;
    } else {
        dlssg = 0, framegen = row < 0 ? 0 : row;
    }
}
/// The row's choices here, in order: Off, Auto, 2x to 8x; AMD FSR 4 where its library makes the frames; NVIDIA DLSS 2x to 4x
/// where DLSS frame generation can run. out holds 13 at most; their count
inline int framegen_choices(int* out, bool fsr4_offered, bool dlss_offered) {
    int n = 0;
    for (int v = 0; v <= 8; v++) out[n++] = v;
    if (fsr4_offered) out[n++] = 9;
    for (int v = FG_DLSS; dlss_offered && v <= FG_DLSS_LAST; v++) out[n++] = v;
    return n;
}

// ---- Frame pacing: Off | Even (opts.pace 1) | Steady, any rate | Steady, refresh steps (opts.steady_fps 1 and 2, host/pace_steady.h:
// read at the start, a restart). Steady shows frames on an even line too, so it keeps frame_pacing on ----
enum { PACE_OFF = 0, PACE_EVEN = 1, PACE_STEADY_ANY = 2, PACE_STEADY_STEPS = 3 };
constexpr int pacing_row(int pace, int steady) {
    return steady == 1 ? (int)PACE_STEADY_ANY : steady == 2 ? (int)PACE_STEADY_STEPS : pace ? (int)PACE_EVEN : (int)PACE_OFF;
}
inline void pacing_set(int row, int& pace, int& steady) {
    pace = row != PACE_OFF;
    steady = row == PACE_STEADY_ANY ? 1 : row == PACE_STEADY_STEPS ? 2 : 0;
}

// ---- Low latency: Off | On | On + Boost, NVIDIA Reflex on an NVIDIA card (opts.reflex 0 to 2), AMD Anti-Lag 2 on an AMD one
// (opts.antilag 0 or 1: no Boost). No row where neither runs ----
enum Vendor { LATENCY_NONE = 0, LATENCY_NVIDIA = 1, LATENCY_AMD = 2 };
constexpr int latency_hi(Vendor v) { return v == LATENCY_NVIDIA ? 2 : v == LATENCY_AMD ? 1 : 0; }
constexpr int latency_row(Vendor v, int reflex, int antilag) {
    return v == LATENCY_NVIDIA ? (reflex < 0 ? 0 : reflex > 2 ? 2 : reflex) : v == LATENCY_AMD ? (antilag ? 1 : 0) : 0;
}
inline void latency_set(int row, Vendor v, int& reflex, int& antilag) {
    if (v == LATENCY_NVIDIA) reflex = row < 0 ? 0 : row > 2 ? 2 : row;
    else if (v == LATENCY_AMD) antilag = row > 0;
}

/// The next of a row's choices (ascending) from v, dir +1 or -1, wrapping. A value that is not among them (saved where it
/// could run) steps to its neighbours by value
inline int step_in(const int* choices, int n, int v, int dir) {
    if (n <= 0) return v;
    int i = 0;
    while (i < n && choices[i] < v) i++;
    if (i < n && choices[i] == v) return choices[(i + (dir > 0 ? 1 : n - 1)) % n];
    return dir > 0 ? choices[i % n] : choices[(i + n - 1) % n];
}

// ---- The options file from before version 8 ----
constexpr int kVersion = 8;            // (vk_menu.cpp save writes it)
constexpr int kHandheldOutput = 7;     // (the Output resolution row's 1536x864, the game's handheld profile: opts.output_res 7)
/// The keys a file before version 8 can have that the menu no longer writes, each row's job done otherwise: quality_preset (its
/// memory limits are the card's own, sized from its video memory), upscaling (the window's filter follows the sizes:
/// upscaler_rows.h auto_filter), reflex_frame_limit (off, its old default, until it has been played: RX_VK_REFLEX_LIMIT=1
/// for a run, vk_menu.h reflex_limit), handheld_profile (an Output
/// resolution), stick_dead_zone (each stick's own row). save drops their lines
constexpr const char* kDropped[] = {"quality_preset", "upscaling", "reflex_frame_limit", "handheld_profile", "stick_dead_zone"};
constexpr int kDroppedCount = sizeof kDropped / sizeof kDropped[0];
/// What a file before version 8 had in those keys that 8 keeps elsewhere (the struct keeps its first name)
struct Before7 {
    int handheld = 0;       // (handheld_profile=)
    int stick_shared = 0;   // (stick_dead_zone=: 0 XInput's ~24%, 1 to 5 the shares below)
};
/// The stick dead zone row's shares, in percent (host/nn_hid.c hos_stick_axis's cuts: a stick's own of the same percent cuts
/// at the same place, hos_stick_axis_own); -1 XInput's own, the Left and Right stick dead zones' Default
constexpr int kSharedZone[6] = {-1, 5, 10, 15, 20, 30};
/// A file from before version 8 as 8 has it, the same in play: the handheld profile as the Output resolution's handheld
/// size, the shared stick dead zone in each stick's own row where it had none of its own
inline void migrate(int version, const Before7& old, int& output_res, int& lstick_dead_zone, int& rstick_dead_zone) {
    if (version >= kVersion) return;
    if (old.handheld) output_res = kHandheldOutput;
    const int shared = old.stick_shared > 0 && old.stick_shared < 6 ? kSharedZone[old.stick_shared] : -1;
    if (lstick_dead_zone < 0) lstick_dead_zone = shared;
    if (rstick_dead_zone < 0) rstick_dead_zone = shared;
}

} // namespace mkwvk::merged_rows
