// The Insert menu. Drawn with Dear ImGui over the window's image at the last step of a present (menu_record; vk_imgui.h, in the
// game's style: vk_ui.h), so it costs nothing while closed and it never touches the game's picture, nor what frame interpolation
// sees. The FPS counter and the start-up screen are drawn the same way. F1 always opens and closes it, Insert too by default.
// Up and Down choose, Left and Right change, Esc closes; the game gets no keyboard input while it is open.
// (hookshot) A gamepad's Home button, or Select and Start together, opens it too; the D-pad chooses and changes, the button
// the Switch's A is bound to changes too (and opens a page), the one B is bound to goes back (vk_menu_keys.h). The game gets no gamepad input while it is open either.
// Its Controls page binds the Switch's buttons to gamepad inputs and keys.
// (hookshot) The mouse is free while it is open (the window lets it go: WM_APP + 8, host/display.c and
// host/posix/display_sdl.c): the row under the cursor is selected, a click on a tab shows it, on a value's left or right
// half changes it down or up, on a toggle flips it, on a slider's track sets it (or drag it); the wheel scrolls the rows
// (never a value, wherever the cursor is); a right click is Esc. On the Cheats page and its lists a click is Enter (on a
// choice's or an amount's left or right half, Left or Right) and a right click goes back. The drawing records each control's
// place and the mouse is tested against that (vk_menu_hit.h).
// (hookshot) The rows that take a restart (Row::restart) are tagged "After restart" while their value is not the one the game
// started with, and the Graphics tab's help lists them. Closing the menu then asks whether to restart now (menu_restart.h):
// Later keeps the saved settings for the next start and is not asked again until they change; Restart now saves and
// closes the window as the player would, and the game starts again (WM_APP + 9: host/hos_relaunch.c).
// (Triforce, 11-10) The menu is drawn as the game's own Options screen (SystemOption_00 of its layout archive): the System
// screens' header with the options' categories across it as the pause menu has its own (Display, Graphics, Audio, Controls,
// System; L and R, Q and E), the rows on the game's plates in its list column, the caption plate at the right, the guides at
// the foot, in the game's fonts and with its button icons (vk_ui.h, vk_game_tex.h, vk_imgui.cpp: all read from the player's
// game files at run time). The Debug tab is always shown (11-15, the owner: "make debug on by default and remove the alt+g
// shortcut"; Alt+G unlocked it before). The Menu size row is gone: the game's menus have one size. The FPS counter is a HUD counter (the
// HUD's own digits, no plate). tests/ui_preview.cpp draws all of it into pictures without the game.
#include <atomic>
#include <chrono>
#include <cmath>
#include <utility>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>   // ((Triforce) the name pages' read of the game's files)
#include <vector>

#include "vk_dlssg.h"
#include "vk_pace.h"      // (Triforce: who holds the frames, for the rows that hand them to another limiter)
#include "vk_bugshot.h"   // (Triforce: a bug capture's picture, taken before the overlays are drawn)
#include "vk_paths.h"
#include "vk_menu.h"
#include "vk_menu_hit.h"
#include "vk_draw.h"   // (pipeline_cache_save: Restart now keeps what was built)
#include "oot_cheat_items.h"
#include "debug_names.h"   // ((Triforce) the Debug tab's name pages: their lists, and oot_debug_link.h's lines)
#include "debug_tab_layout.h"   // ((Triforce) the Debug tab's cells' widths)
#include "debug_pending.h"      // ((Triforce) the Inventory's change waiting for the game)
#include "vk_progress.h"   // (StartupView, version_text: the start-up screen)
#if __has_include("vk_startup_draw.h")   // (the shader-screen branch's file: with it the start-up screen is its drawing)
#include "vk_startup_draw.h"   // (the start-up screen's drawing: draw_startup_screen)
#endif
#include "vk_ui.h"
#include "vk_menu_keys.h"   // (which gamepad buttons confirm and go back: those A and B are bound to)         // (how it looks: Dear ImGui, vk_imgui.h)
#include "vk_dlss.h"
#include "vk_fsr3.h"      // (the Upscaler row: whether AMD FSR 3.1 runs on this GPU)
#include "vk_ffx3.h"      // (...or AMD's own library draws it)
#include "vk_fsr3fi.h"    // (the Frame generation row: whether FSR 3.1 makes the frame)
#include "vk_fsr4.h"
#include "vk_ngxfg.h"    // (the Frame generation row: whether DLSS Frame Generation makes the frames)
#include "vk_ffx_fg.h"    // (the Frame generation engine row: whether AMD's library can make the frames)
#include "vk_framegen.h"   // (the Frame generation row's hint: which engine makes the frames, framegen_choice)
#include "vk_temporal.h"
#include "vk_fsr3_inputs.h"   // (Triforce: the render size while AMD FSR 3.1 draws, for the performance overlay)
#include "perf_stats.h"       // (Triforce: the performance overlay's frame times)
#include "perf_machine.h"     // (Triforce: and the machine's numbers, sampled on a thread of their own)
#include "vk_renodx.h"    // (renodx_built: the RenoDX rows only in a build with its shaders)
#include "vk_reflex.h"
#include "vk_antilag.h"
#include "upscaler_rows.h"
#include "aa_rows.h"        // (Triforce, the Anti-aliasing row: its choices, and when it does not apply)
#include "vk_aa.h"          // (...RX_VK_AA over it, and whether the game's pass was there to replace)
#include "tex_detail.h"     // (the Texture detail row: the levels' names, RX_VK_TEX_DETAIL)
#include "fps_cap.h"        // (the Game frame rate cap row's Monitor's rate)
#include "ultrawide_rows.h" // ((Triforce) the Ultrawide row's shapes, and its old Monitor's shape read before options_version 6)
#include "present_mode.h"   // (the VSync row: what its choice comes to on this driver)
#include "game_language.h"  // (the Game language row: what language.txt chooses and offers)
#include "menu_restart.h"
#include "mod_cheat_rows.h"   // (Triforce: the System tab's rows for the cheats of the mods' cheat files)
#include "menu_sound_events.h"   // (Triforce: the options' own sounds: which one a tick asks for, and their queue to the host)
#include "vk_mods.h"   // (Triforce: the Mods page's engine, GameBanana's list and the installs)
#include "vk_salesman.h"   // (Triforce: the Happy Mask Salesman beside the cards)
#include "imgui_internal.h"   // (RegisterUserTexture: the mods' pictures)
#include "ini_merge.h"   // ((Triforce, handoff §9) the options file's lines merged by key with a change made outside the game)
#include "file_whole.h"  // (and the file written whole: file_writers::write_whole)
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)
#ifndef HOOKSHOT_VERSION   // (nx2/build.py gives it to this file: nx2/version.py)
#define HOOKSHOT_VERSION "unknown"
#endif

namespace mkwvk {
HWND app_window();
int texture_detail_auto();   // (vk_texture.cpp: what the video memory gives this card, -1 before the first texture)
int display_refresh_mhz();   // (vk_main.cpp: the window's display, mHz; 0 when not known)
int swapchain_present_mode();   // (vk_main.cpp: the swapchain's present mode, present_mode.h's numbers; -1 none)
// ((Triforce) the performance overlay on Full: the rendering GPU's LUID (vk_main.cpp), the GPU's time in the recordings and their
// uploads so far (vk_replay.cpp), the textures uploaded so far (vk_texture.cpp), whether frame generation makes frames now
// (vk_framegen.cpp))
bool gpu_device_luid(uint8_t out[8]);
int64_t gpu_time_busy_ns();
void texture_stats(int64_t* uploads, int64_t* astc_gpu, int64_t* bytes_uploaded);
bool framegen_on();

Options opts;

namespace {

// (Triforce) The tabs across the Options screen's header, as the pause menu's categories: a kind of option each. Debug is the
// last, always shown (debug_shown)
enum MenuTab {
    TAB_DISPLAY = 0,
    TAB_GRAPHICS = 1,
    TAB_AUDIO = 2,
    TAB_CONTROLS = 3,
    TAB_SYSTEM = 4,
    TAB_DEBUG = 5,   // (the OoT debug tools of #785 and the developer's rows, a tab of their own)
    TAB_COUNT = 6,
    TAB_MOTION = TAB_COUNT,   // (hookshot: not a tab of the strip, the Motion page that the Controls tab's Motion row
                              // opens: one past the strip's tabs, so a tab added before TAB_COUNT moves it along)
    TAB_PAGE = TAB_MOTION + 1   // (hookshot: the Controls page's rows, above its grid; no tab shows them)
};

struct Row {
    MenuTab tab;
    const char* name;
    int* value;
    int lo, hi, step;
    const char* (*text)(int v);
    const char* help;   // (shown under the list while the row is selected: what the row is, the same whatever its value)
    // (a line about the value chosen, under the help, or nullptr: hint(v, -1) is the one to show now for v, hint(v, 0),
    // hint(v, 1), ... every one v can have (those that depend on the other rows or on the GPU), until nullptr, so the
    // menu keeps the height of the tallest: measure_help)
    const char* (*hint)(int v, int variant) = nullptr;
    bool restart = false;   // (read once, at the start: a change applies after a restart)
    bool preset_exempt = false;   // (a change here leaves the Quality preset alone: the controls, the window, the counter,
                                  // frame generation)
    signed char cheat = -1;   // (a debug tool, on the Debug tab with the tools on: its index in the cheat_ tables, -1 none)
    bool heading = false;     // (a section's title on its tab, not a row: the selection steps over it)
};
/// A row's hints from a table, the value lo first (nullptr: none for that value)
const char* hint_in(const char* const* t, int n, int lo, int v, int variant) {
    return variant <= 0 && v - lo >= 0 && v - lo < n ? t[v - lo] : nullptr;
}
#define MENU_HINTS(fn, lo, ...)                                                                                              \
    const char* fn(int v, int variant) {                                                                                     \
        static const char* const t[] = {__VA_ARGS__};                                                                        \
        return hint_in(t, (int)(sizeof t / sizeof t[0]), lo, v, variant);                                                    \
    }
const char* tx_framegen(int v) {
    // (Triforce: the last is AMD's frame generation library, vk_fsr4.h fsr4fg. A saved AMD FSR 4 on a PC where the library
    // cannot make the frames shows what runs there, 2x: framegen_rows.h shown)
    static const char* t[] = {"Off", "Auto", "2x", "3x", "4x", "5x", "6x", "7x", "8x", "AMD FSR 4"};
    return t[std::clamp(framegen_rows::shown(v, fsr4fg::offered_known()), 0, (int)framegen_rows::kRowMax)];   // ((Triforce) as known: menu_ahead)
}
const char* tx_onoff(int v) { return v ? "On" : "Off"; }
const char* tx_dlssg(int v) { return v == 1 ? "2x" : v == 2 ? "3x" : v == 3 ? "4x" : "Off"; }
/// (what runs in this session, or why not: vk_dlssg.cpp. Variant 0 stands for the longest line, for the menu's height)
const char* hn_dlssg(int v, int variant) {
    if (variant > 0) return nullptr;
    if (variant == 0) return "Not on this graphics card or driver: it needs an RTX 40 or 50 card.";
    if (!v) return "Off.";
    return mkwvk::dlssg::asked() ? mkwvk::dlssg::status() : "Starts with the next start of the game.";
}
/// (Triforce) The Reflex frame limit row: what holds the frames now. Variant 0 stands for the longest line
/// (Triforce) RivaTuner Statistics Server, where it holds the frames, is the limiter whatever this row says (vk_pace.cpp)
const char* hn_reflex_limit(int v, int variant) {
    static const char* const off = "The game's own wait holds every frame, or RivaTuner where it is the game's limiter.";
    static const char* const none = "DLSS frame generation does not run in this session: the game's own wait holds every frame.";
    static const char* const rtss = "RivaTuner Statistics Server holds the frames now, and Reflex is given no limit.";
    if (variant > 0) return nullptr;
    if (variant == 0) return none;
    if (v && mkwvk::pace::owner() == mkwvk::pace::RTSS) return rtss;
    return !v ? off : mkwvk::dlssg::active() ? "Reflex holds the frames while they are generated." : none;
}
const char* tx_menu_shortcut(int v) {
    static const char* const names[] = {"Insert or F1", "F1 only"};   // (F1 is never turned off: the menu is always one key away)
    return names[v ? 1 : 0];
}
const char* tx_hdr(int v) { return v == 1 ? "Expanded" : v == 2 ? "Native" : "Off"; }   // ((Triforce, 11-16) "Native", the owner's word)
const char* tx_nits(int v) {
    static char b[24];
    snprintf(b, sizeof b, "%d nits", v);
    return b;
}
static_assert(fps_cap::kMonitor == 6, "the Game frame rate cap row's last value is Monitor's rate");
const char* tx_fps_cap(int v) {
    static const char* const t[] = {"60 (the console's)", "90", "120", "144", "165", "240"};
    if (v < 0) return v == -1 ? "45" : v == -2 ? "40" : "30";   // (Triforce: the limits under 60, host/nn_vi.c fps_max)
    if (v >= fps_cap::kMonitor) {   // (Triforce: Monitor's rate, with what this display gives now: fps_cap.h, as the host asks it)
        static char b[32];
        const int cap = fps_cap::of_display(display_refresh_mhz());
        if (!cap) return "Monitor's rate";
        snprintf(b, sizeof b, "Monitor's rate (%d)", cap);
        return b;
    }
    return t[v];
}
// (Triforce) The Steady frame rate row (host/pace_steady.h's PACE_STEADY_*)
const char* tx_steady_fps(int v) { return v == 1 ? "Any rate" : v == 2 ? "Refresh steps" : "Off"; }
// (Triforce) The VSync row (opts.vsync; present_mode.h has what On and Off ask for): the line under its help says what the
// choice came to on this driver, by the present mode of the swapchain there is, or which switch decides for the run.
// Variants (every one): each mode the value can come to, then the two switches
const char* hn_vsync(int v, int variant) {
    static const char* const on[] = {
        "Each frame waits for the screen's next refresh: no tearing.",
        "The newest finished frame at each refresh of the screen: no tearing, and nothing held back.",
        "This driver has no tear-free mode that does not hold the frame rate down: frames are shown at once, as with Off.",
    };
    static const char* const off[] = {
        "This graphics driver cannot show frames at once here: as On.",
        "This graphics driver cannot show frames at once here: as On.",
        "Each frame is shown the moment it is ready: a little less delay, and the picture can tear.",
    };
    static const char* const fifo_env = "Set for this run by RX_VK_FIFO: each frame waits for the screen's next refresh.";
    static const char* const immediate_env = "RX_VK_IMMEDIATE asks for frames shown at once in this run, whatever is chosen here.";
    if (variant < 0) {
        static const bool fifo = getenv("RX_VK_FIFO") != nullptr, immediate = getenv("RX_VK_IMMEDIATE") != nullptr;
        if (fifo || immediate) return fifo ? fifo_env : immediate_env;
        const int mode = swapchain_present_mode();
        if (mode < present_mode::kFifo || mode > present_mode::kImmediate) return nullptr;   // (no swapchain: nothing to say)
        return (v ? on : off)[mode];
    }
    if (variant <= present_mode::kImmediate) return (v ? on : off)[variant];
    return variant == 3 ? fifo_env : variant == 4 ? immediate_env : nullptr;
}
// (hookshot) Motion controls: the choice, and whether a connected controller has a gyroscope (host/pad_sdl.c). Either can be
// chosen whatever is connected: a pad plugged in later is read when it comes
const char* tx_gyro(int v) {
    const int status = host && host->gyro_status ? host->gyro_status() : -1;
    if (status == 2) return v ? "On (gyro found)" : "Off (gyro found)";
    if (status == 1) return v ? "On (no gyro on this pad)" : "Off (no gyro on this pad)";
    if (status == 0) return v ? "On (no controller)" : "Off (no controller)";
    return v ? "On" : "Off";
}
const char* tx_mousecam(int v) { return v ? "Native" : "As the stick"; }
const char* tx_sens(int v) {
    static char b[12];
    snprintf(b, sizeof b, "%.2f", v / 100.0);
    return b;
}
// (the game's picture and the window it is shown in, as the last present had them: menu_note_present_sizes)
std::atomic<uint64_t> present_sizes{0};
bool filter_needed() {
    const uint64_t v = present_sizes.load(std::memory_order_relaxed);
    return upscaler_rows::filter_needed((unsigned)(v >> 48), (unsigned)(v >> 32) & 0xffff, (unsigned)(v >> 16) & 0xffff,
                                        (unsigned)v & 0xffff);
}
const char* tx_upscale(int v) {
    static const char* t[] = {"Off", "Catmull-Rom", "Lanczos", "AMD FSR 1"};
    if ((unsigned)v >= sizeof t / sizeof t[0]) return t[0];   // (a setting past the row: the label, not the next bytes)
    return t[v];
}
/// The Window scaling filter row's value: its filter, or that none is needed (the saved value stays, for another window size)
const char* tx_filter(int v) { return filter_needed() ? tx_upscale(v) : "Not needed (window = output)"; }
const char* tx_reflex(int v) {
    static const char* t[] = {"Off", "On", "On + Boost"};
    // ((Triforce, 11-16: the owner saw Off and "not available" with DLSS frame generation on an NVIDIA card) With DLSS frame
    // generation Reflex is Streamline's and always on (vk_dlssg.cpp reflex_apply): its Off shows On, Boost as chosen
    if (mkwvk::dlssg::reflex_on()) return t[std::clamp(v, 1, 2)];
    return t[reflex_available() ? std::clamp(v, 0, 2) : 0];
}
/// Reflex: on the value, that it runs through Streamline with DLSS frame generation, or that it is not available here (the
/// row shows Off)
const char* hn_reflex(int v, int variant) {
    static const char* const t[] = {"The game as without it.", "The driver paces the frames for less latency.",
                                    "As On, and the GPU's clocks are kept up as well."};
    static const char* const none = "Not available here: it needs an NVIDIA GPU whose driver has VK_NV_low_latency2.";
    static const char* const sl = "With DLSS frame generation Reflex runs through NVIDIA Streamline and stays on, as frame "
                                  "generation needs it. On + Boost keeps the GPU's clocks up as well.";
    if (variant < 0) return mkwvk::dlssg::reflex_on() ? sl : reflex_available() ? t[std::clamp(v, 0, 2)] : none;
    return variant == 0 ? t[std::clamp(v, 0, 2)] : variant == 1 ? none : variant == 2 ? sl : nullptr;
}
const char* tx_antilag(int v) { return antilag_available() && v ? "On" : "Off"; }
/// Anti-Lag 2: on the value, or that it is not available here (the row shows Off)
const char* hn_antilag(int v, int variant) {
    static const char* const t[] = {"The game as without it.", "The driver delays each frame's start for less latency."};
    static const char* const none = "Not available here: it needs an AMD GPU whose driver has VK_AMD_anti_lag.";
    if (variant < 0) return antilag_available() ? t[v ? 1 : 0] : none;
    return variant == 0 ? t[v ? 1 : 0] : variant == 1 ? none : nullptr;
}
// The Upscaler (opts.dlss): what scales the 3D up to the output. DLSS and DLAA only where dlss_why() lets them run, FSR 3.1
// only where vk_fsr3.cpp says it does, FSR 4 and its native AA only where fsr4_why() does; the menu shows (and steps from)
// what runs, and keeps the saved choice for a PC where it can
namespace ur = upscaler_rows;
/// One bit per upscaler that runs here (upscaler_rows.h bit). Each backend caches its own answer, so this costs nothing
unsigned upscalers_available() {
    return ur::bit(ur::GAME) | ur::dlss_bits(dlss_why() == DLSS_OK) | ur::fsr3_bits(fsr3::supported() || ffx3_usable()) |
           ur::fsr4_bits(fsr4_why() == FSR_OK);
}
/// (FSR 3.1 has inputs of its own, at the Render resolution's mode or as its Native AA; DLSS and FSR 4 run on the frame
/// chain they share, temporal_mode)
int upscaler() { return fsr3_wanted() ? (int)(fsr3_native() ? ur::FSR3_AA : ur::FSR3) : temporal_mode(); }
const char* tx_upscaler(int) {
    static const char* t[ur::kUpscalers] = {"Game", "NVIDIA DLSS", "NVIDIA DLAA", "AMD FSR 3.1", "AMD FSR 4",
                                            "AMD FSR 4 Native AA", "AMD FSR 3.1 Native AA"};
    return t[std::clamp(upscaler(), 0, ur::kUpscalers - 1)];
}
/// The Upscaler: the one shown (what runs, upscaler()), and with the game's own, why DLSS and FSR are not offered. Variants
/// (every one): each value, then the game's own with each of DLSS's reasons, then with each of FSR's
const char* hn_upscaler(int v, int variant) {
    static const char* const t[ur::kUpscalers] = {
        "The game's own network.",
        "NVIDIA's, from a smaller 3D. Softer than the game's own: set Sharpness to around 80%. Things that move by "
        "themselves can trail for now.",
        "NVIDIA's, the 3D at full size with its edges smoothed; the slowest. Softer than the game's own: set Sharpness to "
        "around 80%. Things that move by themselves can trail for now.",
        "AMD's, from a smaller 3D, on any GPU: built from AMD's own shaders. Things that move by themselves can trail for "
        "now.",
        "AMD's machine-learned upscaler, from a smaller 3D, with AMD's files beside the game. Things that move by themselves "
        "can trail for now.",
        "AMD's machine-learned upscaler, the 3D at full size with its edges smoothed; the slowest.",
        "AMD's, the 3D at full size with its edges smoothed, on any GPU: built from AMD's own shaders. Slower than FSR 3.1 "
        "from a smaller 3D; the 3D takes its full size at the next start. Things that move by themselves can trail for now.",
    };
    static const char* const dlss_no[DLSS_WHY_COUNT] = {
        nullptr,
        "DLSS and DLAA: not in this build (built without NVIDIA's DLSS SDK, see the readme).",
        "DLSS and DLAA: off for this run (RX_VK_DLSS=0).",
        "DLSS and DLAA: need an NVIDIA RTX GPU.",
        "DLSS and DLAA: need a newer NVIDIA driver.",
        "DLSS and DLAA: DLSS's runtime was not found (the game folder's ngx folder, see the readme).",
        "DLSS and DLAA: NVIDIA's NGX on Linux loads DLSS's runtime only from a folder named in ASCII (see the readme).",
        "DLSS and DLAA: NVIDIA's NGX started but gave DLSS nothing to work with (see the log).",
    };
    static const char* const fsr_no[FSR_WHY_COUNT] = {
        nullptr,
        "AMD FSR 4: not in this build (see the readme).",
        "AMD FSR 4: off for this run (RX_VK_FSR4=0).",
        "AMD FSR 4: AMD's files were not found beside the game (see the readme).",
        "AMD FSR 4: AMD's files have no FSR 4 for this GPU.",
    };
    // (now: the value shown and what runs here, with both engines' reasons; every one: as said above)
    int d = 0, f = 0;
    if (variant < 0) {
        v = std::clamp(upscaler(), 0, ur::kUpscalers - 1), d = (int)dlss_why(), f = (int)fsr4_why();
    } else if (v == ur::GAME) {
        if (variant < DLSS_WHY_COUNT) d = variant;
        else if (variant < DLSS_WHY_COUNT + FSR_WHY_COUNT - 1) f = variant - DLSS_WHY_COUNT + 1;
        else return nullptr;
    } else if (variant > 0) {
        return nullptr;
    }
    v = std::clamp(v, 0, ur::kUpscalers - 1);
    // (the reasons are said only where the engine is in the build: one without FSR does not tell an NVIDIA user why)
    const char* dn = dlss_built() || variant >= 0 ? dlss_no[d] : nullptr;
    const char* fn = fsr4_built() || variant >= 0 ? fsr_no[f] : nullptr;
    if (v != ur::GAME || (!dn && !fn)) return t[v];
    static char b[DLSS_WHY_COUNT][FSR_WHY_COUNT][320];
    snprintf(b[d][f], sizeof b[d][f], "%s%s%s%s%s", t[0], dn ? " " : "", dn ? dn : "", fn ? " " : "", fn ? fn : "");
    return b[d][f];
}
/// The DLSS model row (opts.dlss_model): NVIDIA's network by its preset, or RX_VK_DLSS_PRESET's for this run
const char* dlss_preset_env() {
    static const char* const e = getenv("RX_VK_DLSS_PRESET");
    return ur::preset_env(e) ? e : nullptr;
}
const char* tx_dlss_model(int v) {
    static const char* const t[] = {"Transformer (K)", "Transformer 2 (M)", "Transformer 2 (L)", "NVIDIA default"};
    if (const char* e = dlss_preset_env()) {
        static char b[48];
        const unsigned p = ur::dlss_preset(v, e);
        if (p) snprintf(b, sizeof b, "Preset %c (RX_VK_DLSS_PRESET)", ur::preset_letter(p));
        else snprintf(b, sizeof b, "NVIDIA default (RX_VK_DLSS_PRESET)");
        return b;
    }
    return t[std::clamp(v, 0, ur::kModels - 1)];
}
/// The DLSS model: the value chosen; variant 1: the game's own upscales (the row waits for DLSS or DLAA); variant 2: set for
/// this run by RX_VK_DLSS_PRESET
const char* hn_dlss_model(int v, int variant) {
    static const char* const t[] = {
        "NVIDIA's transformer model: the one tested most here.",
        "NVIDIA's second-generation transformer, its pick for Performance: sharper, at a little more GPU time.",
        "The second-generation transformer as NVIDIA tuned it for Ultra Performance, the smallest Render resolutions.",
        "Lets DLSS's runtime pick for each mode: what it picks can change with a runtime update.",
    };
    static const char* const unused = "Used by NVIDIA DLSS and DLAA: choose one in the Upscaler row.";
    static const char* const env = "Set for this run by RX_VK_DLSS_PRESET.";
    if (variant < 0) variant = dlss_preset_env() ? 2 : upscaler() == ur::GAME ? 1 : 0;
    if (variant == 1) return unused;
    if (variant == 2) return env;
    return variant == 0 ? t[std::clamp(v, 0, ur::kModels - 1)] : nullptr;
}
/// The FSR sharpening row (opts.fsr_sharpness): AMD's own sharpening inside FSR 3.1 (through AMD's library) and FSR 4
const char* tx_fsr_sharpness(int v) {
    if (v <= 0) return "Off";
    static char b[8];
    snprintf(b, sizeof b, "%d%%", v);
    return b;
}
/// The FSR sharpening: the value chosen; variant 1: the Upscaler is not one of AMD's library's; variant 2: FSR 3.1 is drawn by Triforce's own
/// passes here (no library), which have no sharpening of their own
const char* hn_fsr_sharpness(int v, int variant) {
    const char* const t = v <= 0 ? "Off: FSR sharpens nothing itself. The Sharpness row below works as it always does."
                                 : "AMD's own sharpening inside FSR, raised further when the 3D is smaller than three quarters of the output. "
                                   "Apart from the Sharpness row, which sharpens the finished picture.";
    static const char* const unused = "Used by AMD FSR 4 and by FSR 3.1 through AMD's library: choose one in the Upscaler row.";
    static const char* const own = "FSR 3.1 is drawn by Triforce's own passes here (no AMD library), which have no sharpening of their own: use the Sharpness row.";
    if (variant < 0) variant = upscaler() == ur::FSR3 && !ffx3_usable() ? 2 : ur::fsr_sharpen_used(upscaler(), ffx3_usable()) ? 0 : 1;
    return variant == 0 ? t : variant == 1 ? unused : variant == 2 ? own : nullptr;
}
/// The Frame generation engine row (opts.fg_engine): who makes the made frames, RX_VK_FRAMEGEN_VENDOR going over it. A choice that cannot
/// run on this machine is skipped by the arrows and says so (a saved one from another PC is kept, and Auto's or Triforce's own is used)
unsigned fg_engines_available() { return fgengine::rows_available(ngxfg::dlssg_capable(), ffxfg::usable()); }
const char* fg_engine_name(int v) {
    static const char* const t[fgengine::kEngineRows] = {"Auto", "NVIDIA DLSS-G", "AMD (library)", "Triforce"};
    return t[std::clamp(v, 0, fgengine::kEngineRows - 1)];
}
const char* tx_fg_engine(int v) {
    static char b[48];
    if (ngxfg::vendor_env_set()) {   // (the env var's vendor has the row's numbers: framegen_engine.h kEngineRows)
        snprintf(b, sizeof b, "%s (RX_VK_FRAMEGEN_VENDOR)", fg_engine_name((int)ngxfg::env_vendor()));
        return b;
    }
    v = std::clamp(v, 0, fgengine::kEngineRows - 1);
    if (fg_engines_available() & (1u << v)) return fg_engine_name(v);
    snprintf(b, sizeof b, "%s (not here)", fg_engine_name(v));
    return b;
}
/// The Frame generation engine: the value chosen, then (NVIDIA and AMD) why it cannot run here, then that RX_VK_FRAMEGEN_VENDOR sets it.
/// The variants run on without a gap: Auto and Triforce 0 and 1, NVIDIA and AMD 0 to 2, then nullptr
const char* hn_fg_engine(int v, int variant) {
    static const char* const t[fgengine::kEngineRows] = {
        "DLSS Frame Generation where it runs, else AMD's library at 2x, else Triforce's own.",
        "Only NVIDIA DLSS Frame Generation (RTX 40 and 50) makes frames: none where it cannot, such as at a multiplier it does not make or in HDR.",
        "AMD FSR 3.1 frame generation through AMD's library at 2x, with its own swapchain. Triforce's own makes the other multipliers.",
        "Triforce's own: AMD FSR 3.1's interpolation at 2x, the optical flow at any other multiplier. Never DLSS Frame Generation or AMD's library.",
    };
    static const char* const no[fgengine::kEngineRows] = {
        nullptr,
        "Not available here: needs an NVIDIA RTX 40 or 50 GPU with DLSS Frame Generation's runtime (see the readme). Auto is used instead.",
        "Not available here: needs AMD's FidelityFX library beside the game and a device with the queues its swapchain needs (see the readme). "
        "Triforce's own is used instead.",
        nullptr,
    };
    static const char* const env = "Set for this run by RX_VK_FRAMEGEN_VENDOR.";
    v = std::clamp(v, 0, fgengine::kEngineRows - 1);
    if (variant < 0)
        variant = ngxfg::vendor_env_set() ? (no[v] ? 2 : 1) : !(fg_engines_available() & (1u << v)) ? 1 : 0;
    if (variant == 0) return t[v];
    if (no[v]) return variant == 1 ? no[v] : variant == 2 ? env : nullptr;
    return variant == 1 ? env : nullptr;
}
const char* tx_aniso(int v) {   // (Triforce: the Texture filtering row)
    static const char* t[] = {"Game", "2x", "4x", "8x", "16x"};   // ((Triforce, 11-16) "Game", the owner's word)
    if ((unsigned)v >= sizeof t / sizeof t[0]) return t[0];
    return t[v];
}
// (Triforce) The Texture detail row (opts.tex_detail, tex_detail.h): Automatic is the card's own level by its video memory,
// which the row says once the first texture was made; RX_VK_TEX_DETAIL goes over the row for a run
int tex_detail_env() {
    static const int e = tex_detail::parse(getenv("RX_VK_TEX_DETAIL"));
    return e;
}
const char* tx_tex_detail(int v) {
    static const char* const t[] = {"Automatic", "Full", "Reduced", "Low"};
    static const char* const picked[] = {"Automatic (Full)", "Automatic (Reduced)", "Automatic (Low)"};
    if ((unsigned)v >= sizeof t / sizeof t[0]) v = 0;
    const int here = texture_detail_auto();
    return v == 0 && here >= 0 && here <= 2 ? picked[here] : t[v];
}
/// The Texture detail: the value chosen; variant 1: set for this run by RX_VK_TEX_DETAIL
const char* hn_tex_detail(int v, int variant) {
    static const char* const t[] = {
        "By this graphics card's video memory: Full from 12 GB (never on integrated graphics), Reduced on 8 and 10 GB cards, "
        "Low under that. The value shows what it gives here.",
        "Every texture whole. On a card with less than 12 GB the video memory can run out, and the frame rate then falls to a "
        "fraction.",
        "The game's large textures at half their size each way, a quarter of their video memory. Text, icons and the HUD stay "
        "whole.",
        "As Reduced, and the largest textures at a quarter of their size each way.",
    };
    static const char* const env[] = {"Full for this run (RX_VK_TEX_DETAIL=full), whatever is chosen here.",
                                      "Reduced for this run (RX_VK_TEX_DETAIL=reduced), whatever is chosen here.",
                                      "Low for this run (RX_VK_TEX_DETAIL=low), whatever is chosen here."};
    if (variant < 0) variant = tex_detail_env() >= 0 ? 1 + tex_detail_env() : 0;
    if (variant == 0) return t[(unsigned)v < sizeof t / sizeof t[0] ? v : 0];
    return variant <= 3 ? env[variant - 1] : nullptr;
}
/// (Triforce) Opaque textures as BC1 (opts.tex_bc1, tex_bc1.h): nothing about the value itself; variants 0 and 1:
/// RX_VK_TEX_BC1 decides for this run, off or on
const char* hn_tex_bc1(int, int variant) {
    static const char* const t[] = {"Off for this run (RX_VK_TEX_BC1=0), whatever is chosen here.",
                                    "On for this run (RX_VK_TEX_BC1), whatever is chosen here."};
    if (variant < 0) {
        static const char* const e = getenv("RX_VK_TEX_BC1");
        return !e || !*e ? nullptr : t[atoi(e) != 0];
    }
    return variant < 2 ? t[variant] : nullptr;
}
const char* tx_percent(int v) {
    static char b[8];
    snprintf(b, sizeof b, "%d%%", v);
    return b;
}
// (local, RenoDX port: vk_renodx.cpp)
const char* tx_rdx_tonemap(int v) { return v == 2 ? "RenoDRT" : v ? "PsychoV35" : "Vanilla"; }   // (Triforce: RenoDRT)
const char* tx_rdx_gamma(int v) { return v == 1 ? "2.2" : v == 2 ? "BT.1886 (2.4)" : "Off"; }   // (Triforce)
const char* tx_rdx_midgray(int v) { return v == 1 ? "Vanilla" : v == 2 ? "Eye adaptation" : "Render"; }
const char* tx_rdx_space(int v) { return v == 1 ? "BT.709" : v == 2 ? "DCI-P3" : "Vanilla"; }
const char* tx_rdx_grade(int v) {
    static char b[8];
    snprintf(b, sizeof b, "%d", v);
    return b;
}
// The output the menu's sizes are shown for: the Output resolution row's (applied at the next start), so the rows below follow a change
// at once; the handheld profile runs the game's own (1536x864)
struct Out {
    uint32_t w, h;
};
// ((Triforce, 11-16, the owner: "Add a resolution of 5120x2160 before 5120x2880") 6 is 5120x2160, after the others so that the
// saved numbers keep their sizes; kOutputOrder shows it between 5120x1440 and 5120x2880)
constexpr Out kOutputs[] = {{2048, 1152}, {2560, 1440}, {3840, 2160}, {1920, 1080}, {5120, 1440}, {5120, 2880}, {5120, 2160}};
constexpr int kLastOutput = std::size(kOutputs) - 1;
int output_choice(int v) { return v >= 0 && v <= kLastOutput ? v : 0; }
// Display the sizes from smallest to largest while keeping the IDs used by saved settings.
constexpr int kOutputOrder[] = {3, 0, 1, 2, 4, 6, 5};
static_assert(std::size(kOutputOrder) == std::size(kOutputs));
int step_output_choice(int id, int dir) {
    const int* pos = std::find(std::begin(kOutputOrder), std::end(kOutputOrder), output_choice(id));
    const int next = std::clamp((int)(pos - std::begin(kOutputOrder)) + dir, 0, kLastOutput);
    return kOutputOrder[next];
}
Out shown_output() {
    return opts.handheld ? Out{1536, 864} : kOutputs[output_choice(opts.output_res)];
}
// The size the game draws the 3D at for a fixed scale of its output, a twelfth of it: every twelfth asked came out exact at the three
// outputs (2048x1152: 0.58 drew 1195x672, 0.6667 1366x768). So the scale the game is given is the twelfth too: 0.67 drew 2615x1471 at
// 3840x2160, not the 2560x1440 of 8/12.
int twelfths(float scale) { return std::min(std::max((int)(scale * 12.0f + 0.5f), 1), 12); }
void game_size(float scale, uint32_t& w, uint32_t& h) {
    const Out o = shown_output();
    const int k = twelfths(scale);
    w = (o.w * k + 11) / 12, h = o.h * k / 12;
}
const char* tx_resolution(int v) {
    static char b[48];
    const int up = upscaler();
    if (up == ur::DLAA) return "Native (DLAA)";
    if (up == ur::FSR4_AA) return "Native (FSR 4)";
    if (up == ur::FSR3_AA) return "Native (FSR 3.1)";
    if (!ur::percent(v)) return "Auto (dynamic)";
    if (const char* mode = ur::scales_by_mode(up) ? ur::dlss_mode_name(v) : nullptr) {
        snprintf(b, sizeof b, "%s (%d%%)", mode, ur::percent(v));
        return b;
    }
    uint32_t w, h;
    game_size(ur::percent(v) / 100.0f, w, h);
    snprintf(b, sizeof b, "%d%% (%ux%u)", ur::percent(v), w, h);
    return b;
}
/// The Render resolution: the value chosen, for the Upscaler shown (every one: each value with each Upscaler)
const char* hn_resolution(int v, int variant) {
    const int up = variant < 0 ? std::clamp(upscaler(), 0, ur::kUpscalers - 1) : variant;
    if (up >= ur::kUpscalers) return nullptr;
    if (up == ur::DLAA)
        return "DLAA draws the 3D at full size: nothing to set here. Choose DLSS or the game's own upscaler to draw it "
               "smaller.";
    if (up == ur::FSR4_AA)
        return "FSR 4's native AA draws the 3D at full size: nothing to set here. Choose FSR 4 or the game's own upscaler "
               "to draw it smaller.";
    if (up == ur::FSR3_AA)
        return "FSR 3.1's native AA draws the 3D at full size: nothing to set here. Choose FSR 3.1 or the game's own "
               "upscaler to draw it smaller.";
    const int p = ur::percent(v);
    if (up == ur::DLSS) {
        switch (p) {
        case 0: return "The game lowers it when the GPU is busy, and DLSS follows. Set Sharpness to around 80%.";
        case 67: return "DLSS's sharpest mode. Set Sharpness to around 80%.";
        case 58: return "Between Quality and Performance. Set Sharpness to around 80%.";
        case 50: return "Faster, and softer. Set Sharpness to around 80%.";
        case 33: return "DLSS's fastest mode, and its softest. Set Sharpness to around 80%.";
        default: return nullptr;   // (not one of DLSS's steps: the row moves to one)
        }
    }
    if (up == ur::FSR3 || up == ur::FSR4) {   // (the same modes, AMD's ratios)
        switch (p) {
        case 0: return "The game lowers it when the GPU is busy, and FSR follows.";
        case 67: return "FSR's Quality mode, 1.5x: its sharpest.";
        case 58: return "FSR's Balanced mode, 1.7x.";
        case 50: return "FSR's Performance mode, 2x: faster, and softer.";
        case 33: return "FSR's Ultra Performance mode, 3x: its fastest, and its softest.";
        default: return nullptr;   // (not one of FSR's steps: the row moves to one)
        }
    }
    if (!p) return "The game lowers it when the GPU is busy.";
    if (p == 100) return "The 3D at the output's full size: the sharpest, and the slowest.";
    return "A steady size: lower is faster, and softer.";
}
/// (Triforce) The Anti-aliasing row (aa_rows.h, vk_aa.h): its choice, RX_VK_AA's for the run over it; while DLSS or FSR
/// draws, whose anti-aliasing the picture has instead
const char* tx_aa(int v) {
    const int up = upscaler();
    if (ur::is_dlss(up)) return "DLSS's own";
    if (aa_rows::upscaler_own(up)) return "FSR's own";
    return aa_rows::name(aa_rows::chosen(v, aa_env_choice()));
}
/// The Anti-aliasing: the value chosen (variant 0); that it does not apply while NVIDIA DLSS or DLAA (1) or AMD FSR (2)
/// draws; that RX_VK_AA set it for this run (3); that the game's pass was not there to replace (4)
const char* hn_aa(int v, int variant) {
    static const char* const t[aa_rows::kValues] = {
        "The game's own SMAA, as on the console.",
        "None: the 3D's edges as they were drawn. The sharpest, with stair steps on edges and shimmer on thin things.",
        "FXAA in place of the game's SMAA: a fast smoothing of the finished 3D's edges, a little softer on fine detail.",
        "SMAA at its highest preset in place of the game's own: long and diagonal edges smoother, for more GPU time."};
    static const char* const own[2] = {
        "NVIDIA DLSS and DLAA smooth the 3D's edges themselves: nothing to set here. The choice is kept for the game's own "
        "upscaler.",
        "AMD FSR smooths the 3D's edges itself: nothing to set here. The choice is kept for the game's own upscaler."};
    static const char* const env = "Set for this run by RX_VK_AA, whatever is chosen here.";
    static const char* const missing =
        "No frame of the last minute had the game's own anti-aliasing pass to replace (a screen without the 3D, or another "
        "version of the game): the picture is the game's.";
    if (variant < 0) {
        const int up = upscaler();
        const bool asked = aa_rows::chosen(v, aa_env_choice()) != aa_rows::GAME;
        variant = ur::is_dlss(up) ? 1 : aa_rows::upscaler_own(up) ? 2 : asked && aa_state() == aa_rows::MISSING ? 4
                  : aa_env_choice() >= 0 ? 3 : 0;
    }
    switch (variant) {
    case 0: return t[v >= 0 && v < aa_rows::kValues ? v : 0];
    case 1: return own[0];
    case 2: return own[1];
    case 3: return env;
    case 4: return missing;
    default: return nullptr;
    }
}
/// A row with nothing else to choose here: shown without its arrows, Left and Right leave it (and the saved value) alone
bool row_fixed(const int* value) {
    return (value == &opts.reflex && !reflex_available() && !mkwvk::dlssg::reflex_on()) ||
           (value == &opts.antilag && !antilag_available()) ||
           (value == &opts.aa && aa_rows::fixed(upscaler(), aa_env_choice() >= 0)) ||
           (value == &opts.dlss && ur::upscaler_fixed(upscalers_available())) ||
           (value == &opts.resolution && ur::resolution_fixed(upscaler())) || (value == &opts.upscale && !filter_needed()) ||
           (value == &opts.dlss_model && ur::model_fixed(upscaler(), dlss_preset_env() != nullptr)) ||
           (value == &opts.fsr_sharpness && ur::fsr_sharpen_fixed(upscaler(), ffx3_usable())) ||
           (value == &opts.fg_engine && ngxfg::vendor_env_set());
}
const char* tx_output_res(int v) {
    static char b[32];
    v = output_choice(v);
    const Out o = kOutputs[v];
    snprintf(b, sizeof b, "%ux%u", o.w, o.h);   // ((Triforce, 11-16) the size alone; the hint says which is the game's own)
    return b;
}
const char* tx_speed(int v) { return v ? "Real time" : "Per frame"; }
const char* tx_text_speed(int v) {
    static const char* t[] = {"Normal", "2x", "4x", "Instant"};
    if ((unsigned)v >= sizeof t / sizeof t[0]) return t[0];
    return t[v];
}
const char* tx_ultrawide(int v) {   // (Triforce: 1 to 3, the fixed shapes, ultrawide_rows.h; host/hos_main.c ultrawide_ratio)
    static const char* const t[] = {"16:9", "16:10", "21:9", "32:9"};
    return t[std::min(std::max(v, 0), 3)];
}
const char* tx_wide_hud(int v) { return v ? "Screen edges" : "Middle"; }
const char* tx_prompts(int v) {
    static const char* t[HOS_PROMPT_N + 1] = {"Auto", "Switch", "Xbox", "PlayStation 4", "PlayStation 5", "Steam Deck",
                                              "Keyboard & mouse"};
    return t[std::min(std::max(v, 0), (int)HOS_PROMPT_N)];
}
const char* tx_gpu(int v) {
    static char buf[300];
    const int idx = v - 1;
    if (v <= 0) snprintf(buf, sizeof buf, "Auto");
    else if (idx < (int)gpu_device_names.size()) snprintf(buf, sizeof buf, "%d: %s", idx, gpu_device_names[idx].c_str());
    else snprintf(buf, sizeof buf, "%d: Device %d", idx, idx);
    // (the whole name shows: the selected row slides through it, see marquee_pos)
    return buf;
}
const char* tx_pad_layout(int v);
// (hookshot) the Microphone row: Off, the system's default, or one of the host's microphones (host/nn_audio_in.c: the
// ocarina's input streams), told to the host by its name (host->mic_select) whenever it changes. The chosen one is kept by
// its name, mic_want, also while it is unplugged: the row then has one more entry for it, "<name> (not connected)", the
// host lets the default stand in and opens it again once it is back, and the name stays in the ini. In the ini the name is
// cut to kMicNameKept bytes (at a character's start), so the line fits the 80-byte readers of the file (host/nn_hid.c,
// nn_applet.c, hos_main.c), and a name is matched by that much of it
constexpr int kMaxMics = 16;
constexpr size_t kMicNameKept = 60;
std::string mic_want;      // (the chosen microphone's name, cut to kMicNameKept; empty: Off or the default chosen)
bool mic_missing = false;  // (mic_want is not plugged in: the row's last entry, 2 + mic_listed, stands for it)
int mic_listed = 0;        // (the microphones as the host last listed them)
bool can_mic() { return host && host->mic_count && host->mic_name && host->mic_select; }
size_t text_char_start(const std::string& text, size_t at) {
    while (at > 0 && at < text.size() && ((unsigned char)text[at] & 0xc0) == 0x80) at--;
    return at;
}
std::string mic_key(const char* name) {
    std::string k(name ? name : "");
    for (char& c : k)   // (a device's name goes into triforce_options.ini: a line break in it would add lines there)
        if ((unsigned char)c < 0x20 || c == 0x7f) c = ' ';
    if (k.size() > kMicNameKept) k = k.substr(0, text_char_start(k, kMicNameKept));
    // (and no spaces at the end: a saved line's end, made one above, and those an older build saved with the name)
    while (!k.empty() && k.back() == ' ') k.pop_back();
    return k;
}
/// buf cut back to whole UTF-8 characters (snprintf may have cut one)
void utf8_whole(char* buf) {
    size_t n = strlen(buf), i = n;
    while (i > 0 && ((unsigned char)buf[i - 1] & 0xC0) == 0x80) i--;   // (the last character's continuation bytes)
    if (i == 0) return;
    const unsigned char lead = (unsigned char)buf[i - 1];
    const size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (n - (i - 1) < want) buf[i - 1] = 0;
}
const char* tx_mic(int v) {
    static char buf[160];
    if (v <= 0) return "Off";
    if (v == 1) return "Default";
    if (mic_missing && v == 2 + mic_listed) {
        const int room = (int)sizeof buf - (int)sizeof " (not connected)";
        snprintf(buf, (size_t)room + 1, "%s", mic_want.c_str());
        utf8_whole(buf);
        strcat(buf, " (not connected)");
        return buf;
    }
    const char* n = can_mic() ? host->mic_name(v - 2) : "";
    snprintf(buf, sizeof buf, "%s", n && *n ? n : "Microphone");
    utf8_whole(buf);
    return buf;
}
/// The Microphone row: Off, the default, a microphone of the list, or the chosen one while it is unplugged (variant 1)
const char* hn_mic(int v, int variant) {
    if (v <= 0) return variant <= 0 ? "The game hears silence." : nullptr;
    if (v == 1)
        return variant <= 0 ? "The system's default recording device. On Windows, desktop apps must be allowed to use the "
                              "microphone (Settings, Privacy & security)."
                            : nullptr;
    const bool missing = variant < 0 ? mic_missing && v == 2 + mic_listed : variant == 1;
    if (variant > 1) return nullptr;
    return missing ? "Not plugged in: Default stands in until it is back." : "This microphone, by its name. A change applies at once.";
}
// (Triforce) The Game language row. The game's language is the host's to read, once, at the start: language.txt in the
// game folder (host/nn_applet.c desired_language), which Setup writes at a build and which lists the game's languages;
// HOS_LANGUAGE goes over it for a run. The row's entries are the languages that file names (game_language.h), and a choice
// is written back into the file as its choice line, everything else in it as it was (lang_store, at each save). It is not
// kept in triforce_options.ini. No row while the file is not there or names nothing to choose from
namespace gl = game_language;
int lang_entries[gl::kCount];   // (the row's entries: places in gl::kLanguages, in its order)
int lang_count = 0;
int lang_saved = -1;            // (the entry language.txt chooses now: as read at the start, or as last written)
bool lang_failed = false;       // (the last choice could not be written: the row is back on the file's)
const char* tx_language(int v) {
    return lang_count > 0 ? gl::kLanguages[lang_entries[std::min(std::max(v, 0), lang_count - 1)]].name : "";
}
/// The Game language: nothing about the value itself; variant 0: HOS_LANGUAGE decides for this run; variant 1: the choice
/// could not be written
const char* hn_language(int, int variant) {
    static const char* const env = "This run's language is set by HOS_LANGUAGE. The choice here is for runs without it.";
    static const char* const failed = "language.txt in the game folder could not be written: the language stays as it was.";
    if (variant < 0) {
        static const bool by_env = getenv("HOS_LANGUAGE") != nullptr;
        return lang_failed ? failed : by_env ? env : nullptr;
    }
    return variant == 0 ? env : variant == 1 ? failed : nullptr;
}
const char* tx_rumble(int v) { return v ? tx_percent(v) : "Off"; }
const char* tx_volume(int v) { return v ? tx_percent(v) : "Muted"; }
const char* tx_menu_sounds(int v) { return v ? tx_percent(v) : "Off"; }   // (Triforce: the Menu sounds row)
const char* tx_sprint(int v) { return v == 1 ? "Left stick click" : v == 2 ? "B" : "Off"; }
const char* tx_sprint_mode(int v) { return v ? "Hold" : "Toggle"; }
// (hookshot) Menu size: how large the overlays are drawn (vk_ui.h unit); Auto is Large in fullscreen at 1080 lines and up while
// a controller is the menu's input, else Medium
bool pad_last = false;   // (a controller was the menu's last input: its buttons in the prompts)
// (Auto's choice: the controller or not, decided as the menu opens by the input that opened it. A controller's input makes it
// Large at any time; the keys and the mouse never make it smaller while the menu is open, so it never changes size under the
// cursor)
bool auto_pad = false;
int pad_confirm = 0, pad_back = 1;   // (the face buttons that confirm and go back, HOS_PAD_*: menu_keys::confirm_back, each tick)
bool auto_large(uint32_t h) { return opts.fullscreen && h >= 1080 && auto_pad; }
const char* tx_menu_size(int v) {
    static const char* t[] = {"Auto", "Small", "Medium", "Large"};
    if (v == 0) return auto_large(dev.swap_extent.height) ? "Auto (Large)" : "Auto (Medium)";
    return (unsigned)v < 4 ? t[v] : t[0];
}
const char* tx_stick_dead_zone(int v) {
    static const char* t[] = {"Default (24%)", "5%", "10%", "15%", "20%", "30%"};
    return (unsigned)v < 6 ? t[v] : t[0];
}
// (Triforce) a stick's own dead zone: none of its own (the Stick dead zone row's), or 0% to 50%
const char* tx_own_dead_zone(int v) { return v < 0 ? "As Stick dead zone" : tx_percent(v); }
// (Triforce) the Walk key row's keys, in host/nn_hid.c walk_key_code's order
const char* tx_walk_key(int v) {
    static const char* t[] = {"Off", "Caps Lock", "Shift", "Ctrl", "Tab"};
    return (unsigned)v < 5 ? t[v] : t[0];
}
// (hookshot) Audio output: the speakers the game mixes for, read once at its start (host/nn_audio.c au_menu_output)
const char* tx_audio_output(int v) {
    static const char* t[] = {"Auto", "Stereo", "5.1"};
    return (unsigned)v < 3 ? t[v] : t[0];
}
// (hookshot) the motion's deadzone: the console's three zero-drift modes, the rate under which the pad counts as still
const char* tx_deadzone(int v) {
    static const char* t[] = {"Low", "Standard", "High"};
    return t[v < 0 ? 0 : v > 2 ? 2 : v];
}
const char* tx_robust(int v) {
    static const char* t[] = {"Auto", "Off", "On"};
    return (unsigned)v < 3 ? t[v] : t[0];
}
const char* tx_mesh(int v) { return v == 1 ? "Compute fallback" : "Auto"; }   // ((Triforce, 11-18) opts.mesh_mode, vk_main.cpp)
// (Triforce) The Debug tab's Replay helper threads row (opts.replay_threads; vk_replay.cpp par_threads_max): Off, or the
// parallel replay tried on 2 to 8 threads as RX_VK_PARALLEL_AUTO=<n> does. The switches go over the row for a run
const char* replay_threads_env() {
    static const char* const e = getenv("RX_VK_PARALLEL") ? "RX_VK_PARALLEL" : getenv("RX_VK_PARALLEL_AUTO") ? "RX_VK_PARALLEL_AUTO" : nullptr;
    return e;
}
const char* tx_replay_threads(int v) {
    // (Triforce: 8, Automatic, is the count replay_threads.h gives this PC, tried as the fixed counts are)
    static const char* const t[] = {"Off", "Auto, 2 threads", "Auto, 3 threads", "Auto, 4 threads", "Auto, 5 threads",
                                    "Auto, 6 threads", "Auto, 7 threads", "Auto, 8 threads", "Automatic"};
    static_assert(replay_threads_of(1) == 2 && replay_threads_of(7) == 8 && !replay_threads_of(0) && !replay_threads_of(8));
    static_assert(replay_threads::kRowAutomatic == 8 && replay_threads::of_row(8, 3) == 3 && replay_threads::of_row(7, 3) == 8);
    return t[(unsigned)v < sizeof t / sizeof t[0] ? v : 0];
}
/// Replay helper threads: variants 0 and 1: a switch decides for this run; variant 2, Automatic's own line (the row's value
/// unless the options file says otherwise, replay_threads.h)
const char* hn_replay_threads(int v, int variant) {
    static const char* const by_default = "The default. The log's \"replay helper threads\" line at the start says how many this PC "
                                          "gets, and its \"parallel replay, trial\" lines when they are in use.";
    static const char* const t[] = {"Set for this run by RX_VK_PARALLEL, whatever is chosen here.",
                                    "Set for this run by RX_VK_PARALLEL_AUTO, whatever is chosen here.", by_default};
    const bool automatic = v == replay_threads::kRowAutomatic;
    if (variant < 0) {
        const char* e = replay_threads_env();
        return e ? (!strcmp(e, "RX_VK_PARALLEL") ? t[0] : t[1]) : automatic ? t[2] : nullptr;
    }
    return variant < 2 || (variant == 2 && automatic) ? t[variant] : nullptr;
}
const char* tx_preset(int v) {
    static const char* t[] = {"Custom", "Minimum (6GB)", "Low (8GB)", "Balanced (12GB)", "High (16GB)", "Ultra (20GB+)"};
    return t[std::min(std::max(v, 0), 5)];
}
// ((Triforce, 11-15, the owner: "make these say minimum 6gb low 8gb balanced 12gb high 16gb"; Ultra 20GB+, their choice) Each
// preset's budgets are those of its card's size: the 6 GB ones Low (6GB) had, the 8 GB ones Balanced (8GB) had, the 12 GB ones
// High (12GB) had, the 16 GB ones Ultra (16GB+) had, and Ultra's own for 20 GB and more. vk_memory.cpp preset_caps_memory has
// the same sizes)
void sync_preset_budgets(int p) {
    switch (p) {
    case 1: opts.vram_mb = 3600;  opts.mirror_mb = 512;  break;
    case 2: opts.vram_mb = 4800;  opts.mirror_mb = 1024; break;
    case 3: opts.vram_mb = 7500;  opts.mirror_mb = 2048; break;
    case 4: opts.vram_mb = 11000; opts.mirror_mb = 3072; break;
    case 5: opts.vram_mb = 14000; opts.mirror_mb = 3072; break;
    default: opts.vram_mb = 0;    opts.mirror_mb = 0;    break;
    }
}
// (Sharpness: tuned for the window pass's neighbours one picture pixel apart, each about the old setting's sharpening on a
// 3840 x 2160 window at that preset's picture, 2048 x 1152 or the handheld profile's 1536 x 864: same frame, mean |Laplacian|
// of the window and of the picture, on the slider's steps)
void apply_preset(int p) {
    switch (p) {
    case 1: // Minimum (6GB VRAM / iGPU)
        opts.resolution = 4; opts.upscale = 2; opts.sharpness = 30;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 1;
        break;
    case 2: // Low (8GB VRAM)
        opts.resolution = 2; opts.upscale = 2; opts.sharpness = 30;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 1;
        break;
    case 3: // Balanced (12GB VRAM)
        opts.resolution = 1; opts.upscale = 1; opts.sharpness = 30;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 0;
        break;
    case 4: // High (16GB VRAM)
        opts.resolution = 1; opts.upscale = 2; opts.sharpness = 40;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 0;
        break;
    case 5: // Ultra (20GB+ VRAM)
        opts.resolution = 1; opts.upscale = 2; opts.sharpness = 50;
        opts.pace = 1;    opts.realtime = 1; opts.handheld = 0;
        break;
    default:
        break;
    }
    // (the Upscaler, Frame generation, its engine and FSR sharpening are the player's: a preset never turns DLSS or made frames on or off. With DLSS
    // its resolution is DLSS's nearest mode)
    if (p > 0) opts.resolution = ur::snap(upscaler(), opts.resolution);
    // ((Triforce, 11-15, the owner: each preset sets the Texture detail, "default texture detail respectively") Minimum Low,
    // Low Reduced, Balanced, High and Ultra Full: each preset's texture budget is for its card's size, and the smaller ones keep
    // the textures smaller so the budget does not evict and make again what Full would keep asking for. Custom leaves it)
    static const int detail[] = {0, 3, 2, 1, 1, 1};
    if (p > 0 && p <= 5) opts.tex_detail = detail[p];
    sync_preset_budgets(p);
}

MENU_HINTS(hn_preset, 0, "Each row as you set it. Changing most rows yourself comes back to Custom.",
           "Resolution 50% (with DLSS its nearest mode), Lanczos, Sharpness 30%, Handheld profile.",
           "Resolution 75% (with DLSS its nearest mode), Lanczos, Sharpness 30%, Handheld profile.",
           "Resolution 100% (with DLSS its nearest mode), Catmull-Rom, Sharpness 30%.",
           "Resolution 100% (with DLSS its nearest mode), Lanczos, Sharpness 40%.",
           "Resolution 100% (with DLSS its nearest mode), Lanczos, Sharpness 50%.")
MENU_HINTS(hn_gpu, 0, "The best dedicated GPU.")
/// (Triforce) The Frame generation row's AMD FSR 4 (framegen_rows.h FSR4): what makes the frame, or, for a choice saved
/// where it could, why AMD's library cannot here (fsr4fg::why) and what makes the frame in its place. Variants: every line
/// it can show
const char* hn_framegen_fsr4(int variant) {
    static const char* const ok = "One made frame between each two of the game's, by AMD's FSR 4 library from the scene's depth "
                                  "and motion. The HUD is part of the picture it works from.";
    static const char* const no[framegen_rows::FG_WHY_COUNT] = {
        nullptr,
        "AMD FSR 4 is not in this build: the renderer's own makes the one frame, as at 2x.",
        "AMD FSR 4 is switched off for this run (RX_VK_FSR4_FG=0 or RX_VK_FSR4=0): the renderer's own makes the frame.",
        "Not known yet: asked of AMD's library once the game's picture is up.",
        "AMD's files were not found beside the game: the renderer's own makes the one frame, as at 2x.",
        "AMD's frame generation library was not found beside the game: the renderer's own makes the frame.",
        "AMD's library has no FSR 4 frame generation for this GPU and driver: the renderer's own makes the frame.",
        "AMD's frame generation failed in this run (the log says where): the renderer's own makes the frame."};
    if (variant < 0) {
        const int w = (int)fsr4fg::why_known();   // ((Triforce) as known: "not known yet" until menu_ahead has asked)
        return w == framegen_rows::FG_OK ? ok : no[std::clamp(w, 1, (int)framegen_rows::FG_WHY_COUNT - 1)];
    }
    return variant == 0 ? ok : variant < (int)framegen_rows::FG_WHY_COUNT ? no[variant] : nullptr;
}
/// Frame generation: how many frames are made for each of the game's
const char* hn_framegen(int v, int variant) {
    if (v == framegen_rows::FSR4) return hn_framegen_fsr4(variant);   // (Triforce, Windows only: AMD's FSR 4 library makes the frame)
    // The engine that makes the frames (framegen_engine.h: NVIDIA DLSS Frame Generation where it runs, else AMD FSR 3.1 through AMD's
    // library at 2x, else the renderer's own: AMD's passes as ours at 2x, the optical flow otherwise) has its own text, the one
    // the row shows live (variant < 0). The variants all run on without a gap, up to the first nullptr (line 1476): 0 the plain
    // text; at 2x then 1 AMD's passes of the renderer's own, 2 AMD's library, 3 NVIDIA's; at any other value 1 NVIDIA's
    static const char* const own = "One made frame between each two of the game's, by AMD FSR 3.1 from the scene's depth and motion.";
    static const char* const lib = "One made frame between each two of the game's, by AMD FSR 3.1 frame generation (AMD's library, its own "
                                   "swapchain) from the scene's depth and motion.";
    const auto nvidia = [](int n) -> const char* {
        if (n == 1)
            return "A frame for each of the display's refreshes, made by NVIDIA DLSS Frame Generation: none made when the game keeps up. Frame pacing does not apply.";
        static char d[9][128];
        snprintf(d[n], sizeof d[n], "%s between each two of the game's, by NVIDIA DLSS Frame Generation from the scene's depth, motion and camera.",
                 n == 2 ? "One made frame" : n == 3 ? "Two made frames" : n == 4 ? "Three made frames" : n == 5 ? "Four made frames"
                 : n == 6 ? "Five made frames" : n == 7 ? "Six made frames" : "Seven made frames");
        return d[std::clamp(n, 2, 8)];
    };
    const auto plain = [](int n) -> const char* {
        if (n <= 0) return "Only the game's own frames: no added delay.";
        if (n == 1) return "A frame for each of the display's refreshes: none made when the game keeps up. Frame pacing does not apply.";
        static char b[9][96];
        snprintf(b[n], sizeof b[n], "%s between each two of the game's.", n == 2 ? "One made frame" : n == 3 ? "Two made frames" : n == 4 ? "Three made frames"
                 : n == 5 ? "Four made frames" : n == 6 ? "Five made frames" : n == 7 ? "Six made frames" : "Seven made frames");
        return b[std::clamp(n, 2, 8)];
    };
    if (variant < 0) {
        if (v < 1) return plain(v);
        switch (framegen_choice(v).engine) {
        case fgengine::Engine::DlssG: return nvidia(v);
        case fgengine::Engine::AmdLibrary: return lib;
        case fgengine::Engine::Fsr3fi: return own;
        default: return plain(v);
        }
    }
    if (variant == 0 || v < 1) return variant == 0 ? plain(v) : nullptr;
    if (v == 2) return variant == 1 ? own : variant == 2 ? lib : variant == 3 ? nvidia(v) : nullptr;
    return variant == 1 ? nvidia(v) : nullptr;
}
MENU_HINTS(hn_pace, 0, "As soon as it is ready.", "At an even interval: less stutter, at up to one frame of delay.")
MENU_HINTS(hn_output_res, 0, "The game's own size.",
           "The sharpest on a 2560x1440 screen: nothing to scale to the window.",
           "The sharpest on a 3840x2160 screen. The game's render targets get a bigger GPU pool, so a full-size 3D fits.",
           "The sharpest on a 1920x1080 screen: the HUD and text pixel for pixel, nothing to scale to the window.",
           "The sharpest on a 5120x1440 ultrawide screen: the whole frame pixel for pixel. The game's render targets get a bigger GPU pool, so a full-size 3D fits.",
           "The sharpest on a 5120x2880 (5K) screen: the whole frame pixel for pixel. The game's render targets get a bigger GPU pool, so a full-size 3D fits.",
           "The sharpest on a 5120x2160 ultrawide screen: the whole frame pixel for pixel. The game's render targets get a bigger GPU pool, so a full-size 3D fits.")
/// The Window scaling filter: its filter, or that none is needed at this window's size
const char* hn_filter(int v, int variant) {
    static const char* const t[] = {"The plain copy.", "Sharper.", "Sharper still; can ring on edges.",
                                    "AMD FSR 1: rebuilds edges as it enlarges, with Sharpness as its own sharpening."};
    static const char* const none = "The window is the Output resolution's size: nothing to scale. The choice is kept for "
                                    "another size.";
    if (variant < 0) return filter_needed() ? t[std::clamp(v, 0, 3)] : none;
    return variant == 0 ? t[std::clamp(v, 0, 3)] : variant == 1 ? none : nullptr;
}
/// Sharpness: with DLSS or DLAA, the amount that suits them
const char* hn_sharpness(int v, int variant) {
    static const char* const dlss = "DLSS and DLAA look softer than the game's own upscaler: around 80% suits them.";
    const bool with_dlss = variant < 0 ? upscaler() == ur::DLSS || upscaler() == ur::DLAA : variant == 1;
    if (variant > 1) return nullptr;
    return with_dlss ? dlss : v == 0 ? "Off: the picture as it is." : nullptr;
}
MENU_HINTS(hn_handheld, 0, "Docked: the Output resolution's size.", "Lower resolutions, lighter on the GPU.")
MENU_HINTS(hn_ultrawide, 0, "The game's own shape.",
           "A 16:10 picture whatever the screen (as a 1920x1200 or 2560x1600 screen), the world a little narrower at the sides: "
           "the window opens in that shape, and a screen of another shape shows it between black bars.",
           "A 21:9 picture whatever the screen (64:27, as a 2560x1080 screen): the window opens in that shape, and a screen of "
           "another shape shows it between black bars. The HUD keeps its 16:9 shape in the middle.",
           "A 32:9 picture whatever the screen (as a 5120x1440 screen): the window opens in that shape, and a screen of "
           "another shape shows it between black bars. The HUD keeps its 16:9 shape in the middle.")
const char* tx_wide_menus(int v) {
    static const char* t[] = {"Full width", "Centred, blurred sides", "Centred, black bars"};
    return t[std::min(std::max(v, 0), 2)];
}
MENU_HINTS(hn_wide_menus, 0, "The menus stretched over the whole screen, as before.",
           "The menus in the middle 16:9; beside them, what the game draws there, out of focus.",
           "The menus in the middle 16:9; black beside them.")
MENU_HINTS(hn_robust, 0, "Off, on every card.", "Can be faster on some NVIDIA cards.", "Always on.")
MENU_HINTS(hn_mesh, 0, "As the game finds best for this GPU: the fallback on AMD's RX 6000 driver, native mesh shaders elsewhere.",
           "Every mesh draw through compute shaders: a little slower. Try it if walls glow or the GPU stops responding on an AMD "
           "card. The shaders are prepared again once.")
MENU_HINTS(hn_fps, 0, nullptr, "With frame generation on, the game's own rate too.",
           "In the counter's place in the top right corner, a see-through panel: the GPU's and the CPU's load, temperatures and memory, "
           "the frame rate with its lows, the frame times and their graph, the waits per frame and the picture's settings. It "
           "measures only while it is shown.")
/// (Triforce) The FPS counter row's third value, Full, after NX2navi's perf_overlay (off, fps, full): 0 and 1 are Off and
/// On as before. The row keeps its name, and with it its key in the options file (fps_counter)
const char* tx_perf(int v) { return v == 1 ? "On" : v >= 2 ? "Full" : "Off"; }
MENU_HINTS(hn_menu_size, 0, "Large in fullscreen on a screen of 1080 lines or more while a controller is in use, else Medium.",
           "Smaller, for a desk.", "The usual size.", "Larger, for a TV across the room.")
MENU_HINTS(hn_speed, 0, "A step each frame: the game slows down whenever it runs below 60 fps.",
           "Normal speed down to 15 fps, by taking longer steps.")
MENU_HINTS(hn_text_speed, 0, "The game's own pace, with its pauses.", "Twice as fast.", "Four times as fast.",
           "Each page at once, as pressing A does.")
MENU_HINTS(hn_audio_output, 0, "5.1 when the default output device has 5.1 or more speakers, else stereo.",
           "Stereo, whatever the device has.", "5.1, whatever the device has. Mixed down on stereo speakers.")
MENU_HINTS(hn_background, 0, "Only while the game's window has the focus.",
           "Also behind other windows, for streaming or other apps. The keyboard and mouse still need the game's window.")
MENU_HINTS(hn_mousecam, 0, "The mouse stands in for the right stick, with its curve and top speed.",
           "The mouse turns the camera itself, one to one, as in a PC game.")
MENU_HINTS(hn_mouse_invert, 0, nullptr, "Moving the mouse forward tilts the camera down.")
MENU_HINTS(hn_gyro, 0, "The sticks alone aim.",
           "Switch Pro controllers and pads in Switch mode, DualSense and DualShock 4. The value says whether the "
           "connected pad has a gyroscope.")
MENU_HINTS(hn_pad_layout, 0, "The Switch's order: A on the right, B at the bottom, X at the top, Y on the left.",
           "A at the bottom (action and talk), B on the right, X on the left, Y at the top.",
           "A at the bottom, B on the right; X and Y as on the Switch.",
           "Bindings of your own, from the Controls page.")
// ((Triforce) Auto's choice is host/prompts.c auto_face's; the keyboard and mouse face is nx2/prompt_keys.py's)
MENU_HINTS(hn_prompts, 0,
           "Those of the first controller, from its name (SDL's). The keys' and the mouse's when the game starts with no "
           "controller; the Switch's for a controller that has no name, as with XInput alone.",
           nullptr, nullptr, nullptr, nullptr, nullptr,
           "Each button shows the first key or mouse button bound to it on this page, moving shows the movement keys, and "
           "the camera shows the mouse.")
// (Triforce) what each walk key is bound to out of the box (host/nn_hid.c key_default), which it stops pressing while chosen
MENU_HINTS(hn_walk_key, 0, "The movement keys always push the stick all the way: Link runs.",
           "Caps Lock presses no button by default. The system still switches capitals on and off with each press.",
           "Shift is B's key by default. While it is the walk key it does not press B: give B another key on the "
           "Controls page.",
           "Ctrl is the left stick press's key by default, which the Sprint button's Left stick click uses. While it is "
           "the walk key it does not press it.",
           "Tab is one of Plus's keys by default. While it is the walk key it does not press Plus; Enter still does.")
// (Triforce) the Lazy write-back row (vk_memory.cpp lazy_decide, wb_lazy_state.h choose)
MENU_HINTS(hn_lazy_write_back, 0, "Copied back after every submission, as always.",
           "Copied back when it is read. Stays off with an AMD card's driver unless RX_VK_LAZY_WB=1 is set, and does nothing "
           "with no VRAM mirrors.")

// (Triforce) The cheats of the mods' cheat files (mods/<name>/cheats/<build id>.txt), which the host reads and runs
// (host/mod_cheats.c) and names for the menu at the first frame (menu_option_set: HOS_OPT_MODCHEAT_*). Each has a row on the
// System tab with the file's own name, On | Off as the toggles; the table below holds mod_cheat_rows::kSlots rows for them and
// a heading, not shown until the host has named its cheats (row_shown). A row has no value of the menu's: its switch is the
// list's, which the host asks for every frame (menu_take_mod_cheat) and keeps for the next start itself
mod_cheat_rows::List mod_cheats;
constexpr char kModCheatHeading[] = "Mod cheats";   // (the heading row's name in the table below: row_shown tells it by it)
constexpr char kModCheatHelp[] =
    "A cheat from a mod's cheat file, in the mods folder. On: its code runs once every frame of the game. Off at first; one you "
    "switch on stays on at the next start.";
/// A row's place among the mods' cheats (its name is the list's own), -1 for every other row
int mod_cheat_slot(const char* name) { return mod_cheats.slot_of(name); }
#define MOD_CHEAT_ROW(i) {TAB_SYSTEM, mod_cheats.names[i], nullptr, 0, 0, 0, nullptr, kModCheatHelp, nullptr, false, true},
#define MOD_CHEAT_ROWS8(i)                                                                                                  \
    MOD_CHEAT_ROW(i) MOD_CHEAT_ROW(i + 1) MOD_CHEAT_ROW(i + 2) MOD_CHEAT_ROW(i + 3) MOD_CHEAT_ROW(i + 4) MOD_CHEAT_ROW(i + 5)  \
    MOD_CHEAT_ROW(i + 6) MOD_CHEAT_ROW(i + 7)
static_assert(mod_cheat_rows::kSlots == 96, "MOD_CHEAT_ROWS8 below is used twelve times: a row for every slot");

Row rows[] = {
    {TAB_DISPLAY, "Screen", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},   // ((Triforce) 11-17: a section)
    // Display: the window and the screen
    {TAB_DISPLAY, "Fullscreen", &opts.fullscreen, 0, 1, 1, tx_onoff,
     "Fills the whole screen with the game, without the window's border. The same as F10 or Alt+Enter.", nullptr, false, true},
    // (the picture's way, top to bottom: the output's size, what fills it from the 3D, the 3D's size, then the window)
    {TAB_DISPLAY, "Output resolution", &opts.output_res, 0, kLastOutput, 1, tx_output_res,
     "The game's whole frame, HUD included, before it is scaled to the window. Docked only. Applies after a restart.",
     hn_output_res, true},
    {TAB_DISPLAY, "GPU device", &opts.gpu_device, 0, 8, 1, tx_gpu,
     "Which graphics card the game runs on. Applies after a restart.", hn_gpu, true, true},
    {TAB_DISPLAY, "Widescreen", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},   // ((Triforce) 11-17: a section)
    {TAB_DISPLAY, "Ultrawide", &opts.ultrawide, 0, 3, 1, tx_ultrawide,
     "The camera's shape: the game's own 16:9, or a fixed 16:10, 21:9 or 32:9 for a screen or a window of that shape. A screen "
     "of another shape shows the picture between black bars. Applies after a restart.", hn_ultrawide, true},
    // (Triforce: host/oot_wide_hud.c)
    {TAB_DISPLAY, "Ultrawide HUD", &opts.wide_hud, 0, 1, 1, tx_wide_hud,
     "With Ultrawide: where the hearts, the item slot and the map sit on a screen wider than 16:9. Middle: in the "
     "middle 16:9, as on a normal screen. Screen edges: out in the screen's corners. Applies after a restart.", nullptr,
     true, true},
    {TAB_DISPLAY, "Ultrawide menus", &opts.wide_menus, 0, 2, 1, tx_wide_menus,
     "With Ultrawide: the pause, item and map menus, and what shows beside them. Loading screens and pre-rendered movies, "
     "which draw no world either, get the same sides; a fade always covers the whole screen. Changes at once.", hn_wide_menus},
    {TAB_DISPLAY, "Frame rate", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},   // ((Triforce) 11-17: a section)
    {TAB_DISPLAY, "Game frame rate cap", &opts.fps_cap, -3, 6, 1, tx_fps_cap,   // (6: Monitor's rate, fps_cap.h kMonitor)
     "Limits frames rendered by the game. Generated frames are additional. Takes a restart. "
     "30, 40 and 45 are for a PC or handheld that holds one of them steadily where 60 swings; the game keeps its speed. "
     "Past 60 is experimental: the game keeps real time, but some cutscenes and bosses can misbehave (Gohma, the intro's "
     "narration). 60 is the safe choice. Monitor's rate is the refresh rate of the screen the game starts on, from 30 to 240.",
     nullptr, true, true},
    // (Triforce: host/pace_steady.h, host/nn_vi.c steady_period. Off until chosen: nobody has run it in the game)
    {TAB_DISPLAY, "Steady frame rate", &opts.steady_fps, 0, 2, 1, tx_steady_fps,
     "Paces the game to a frame rate this PC holds where you are, under the Game frame rate cap, and shows each frame a "
     "period after the last, so that frames come evenly instead of each as soon as it is ready. It goes down quickly where "
     "frames run late and back up slowly where there is room; loads and single hitches do not move it. Gives up some frame "
     "rate for steadiness. Any rate suits a screen with variable refresh (G-SYNC, FreeSync) or VSync off; Refresh steps keeps "
     "to whole divisions of the refresh rate (at 240 Hz: 120, 80, 60 ...) for VSync without variable refresh. Takes a restart.",
     nullptr, true, true},
    // (Triforce, after NX2navi's reflex_limit: vk_dlssg.cpp pace, dlssg_pace.h. Off until chosen: nobody has run it in the game)
    {TAB_DISPLAY, "Reflex frame limit", &opts.reflex_limit, 0, 1, 1, tx_onoff,
     "With DLSS frame generation: NVIDIA Reflex holds the Game frame rate cap where each frame starts, in place of the "
     "game's own wait, so a frame waits once and the controls are read later. The game's own wait is back whenever no "
     "frame is being generated. Where RivaTuner Statistics Server is the game's limiter it holds the frames instead. "
     "Changes at once.", hn_reflex_limit, false, true},
    // (Triforce, after NX2navi's VSync switch: the swapchain's present mode, made again at once. present_mode.h, vk_main.cpp)
    {TAB_DISPLAY, "VSync", &opts.vsync, 0, 1, 1, tx_onoff,
     "Whether a finished frame waits for the screen's next refresh. Off shows it at once: a little less delay, and the picture "
     "can tear. Neither changes the frame rate, which the Game frame rate cap limits. Changes at once.", hn_vsync, false, true},
    // (Triforce, after NX2navi's perf_overlay: a third value, Full, NX2navi's full performance overlay: perf_sample, draw_perf)
    {TAB_DISPLAY, "FPS counter", &opts.show_fps, 0, 2, 1, tx_perf,
     "On: frames per second in the top right corner. Full: a performance overlay in its place, with the GPU's and the CPU's load, "
     "temperatures and memory, the frame rate and its 1% and 0.1% lows, the frame times and their graph, the waits per frame "
     "and the picture's settings.", hn_fps, false, true},
    {TAB_DISPLAY, "High dynamic range", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},   // ((Triforce) 11-17: a section)
    {TAB_DISPLAY, "HDR", &opts.hdr, 0, 2, 1, tx_hdr,
     "HDR10 out on an HDR display (Windows: Use HDR on in the display settings). Expanded: the game's SDR picture, its white at "
     "the paper white and its highlights up to the peak. Native: the game's own HDR, told the display's peak and paper white.",
     nullptr, false, true},
    {TAB_DISPLAY, "HDR paper white", &opts.hdr_white, 80, 400, 10, tx_nits,
     "How bright the picture's white is with HDR on. Around 200 for a normal room; higher in a bright one.", nullptr, false, true},
    // (Triforce: up to HDR10's own 10,000 nits, as NX2navi's hdr_peak_nits; it stopped at 4,000. Every reader takes that much)
    {TAB_DISPLAY, "HDR peak brightness", &opts.hdr_peak, 400, 10000, 100, tx_nits,
     "The brightest highlights with HDR on: set it to your display's peak brightness.", nullptr, false, true},
    {TAB_DISPLAY, "RenoDX", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},   // ((Triforce) 11-17: a section)
    // (Triforce, 11-13: RenoDX is in every build and off until this row turns it on. The scene's format is chosen at the
    // start, host/nn_vi.c hos_scene_rgba16f: a restart)
    {TAB_DISPLAY, "RenoDX HDR", &opts.renodx, 0, 1, 1, tx_onoff,
     "With HDR set to Native: RenoDX's tone mapping, eye adaptation and colour grading in place of the game's own, and its "
     "rows below. Off: the game's own HDR. Applies after a restart. RenoDX is by Carlos Lopez (clshortfuse), under the MIT "
     "license.", nullptr, true, true},
    // (local, RenoDX port: vk_renodx.cpp. Shown with HDR Native, where its shaders run)
    // (Triforce: RenoDRT, the third tone mapper NX2navi's HDR has; the rows of one tone mapper show while it is chosen,
    // rdx_applies. Ranges and steps as NX2navi's menu has them)
    {TAB_DISPLAY, "RenoDX tonemapper", &opts.rdx_tonemap, 0, 2, 1, tx_rdx_tonemap,
     "With HDR Native: how the scene's light is brought to the display. Vanilla: the game's own curve. PsychoV35: RenoDX's, "
     "which keeps hue and saturation into the highlights, up to the peak brightness. RenoDRT: RenoDX's, which carries the "
     "game's SDR look up to the peak by brightness. RenoDX is by Carlos Lopez (clshortfuse), under the MIT license.",
     nullptr, false, true},
    {TAB_DISPLAY, "RenoDX gamma correction", &opts.rdx_gamma, 0, 2, 1, tx_rdx_gamma,
     "Shows the picture as a 2.2 or BT.1886 (2.4) display would, as the game was made to be seen: darker shadows. Off keeps "
     "the sRGB curve. With every tonemapper. PsychoV35 and RenoDRT still end at the peak brightness; Vanilla's highlights "
     "are the game's own, which 2.2 dims a little and 2.4 brightens a little.",
     nullptr, false, true},
    {TAB_DISPLAY, "RenoDX midgray", &opts.rdx_midgray, 0, 2, 1, tx_rdx_midgray,
     "With PsychoV35: what the scene's middle grey is held to. Render: as rendered. Vanilla: where the game's own curve puts it. "
     "Eye adaptation: it follows what is on screen over a few seconds, weighted towards the middle of the view, as eyes adjust.",
     nullptr, false, true},
    {TAB_DISPLAY, "RenoDX match tone slope", &opts.rdx_match_slope, 0, 1, 1, tx_onoff,
     "With PsychoV35: gives the mid tones the contrast of the game's own curve. Off: PsychoV35's own, softer.", nullptr, false, true},
    {TAB_DISPLAY, "RenoDX tone slope", &opts.rdx_tone_slope, 0, 100, 1, tx_rdx_grade,
     "With PsychoV35: the steepness of the tone curve through middle grey. 50 leaves it unchanged.", nullptr, false, true},
    {TAB_DISPLAY, "RenoDX hue clip", &opts.rdx_hue_clip, 0, 100, 5, tx_percent,
     "With PsychoV35: how much bright colours turn as they do where the game's SDR picture clips: fire towards yellow and "
     "white. 0% keeps their hue.", nullptr, false, true},
    {TAB_DISPLAY, "RenoDX UI brightness", &opts.rdx_ui_white, 80, 400, 10, tx_nits,
     "How bright the menus' and the HUD's white is with RenoDX's tonemapper. The scene's follows HDR paper white.", nullptr, false, true},
    {TAB_DISPLAY, "RenoDX color space", &opts.rdx_space, 0, 2, 1, tx_rdx_space,
     "The colours the picture is taken to be in before HDR10's. Vanilla: the game's own matrix. BT.709: as the SDR picture. "
     "DCI-P3: more saturated.", nullptr, false, true},
    {TAB_DISPLAY, "RenoDX exposure", &opts.rdx_exposure, 0, 200, 5, tx_percent,
     "The scene's brightness before RenoDX's tone mapping. 100% is the game's.", nullptr, false, true},
    {TAB_DISPLAY, "RenoDX highlights", &opts.rdx_highlights, 0, 100, 5, tx_rdx_grade,
     "RenoDX's colour grading: the bright parts of the scene. 50 is neutral.", nullptr, false, true},
    {TAB_DISPLAY, "RenoDX shadows", &opts.rdx_shadows, 0, 100, 5, tx_rdx_grade,
     "RenoDX's colour grading: the dark parts of the scene. 50 is neutral.", nullptr, false, true},
    {TAB_DISPLAY, "RenoDX contrast", &opts.rdx_contrast, 0, 100, 5, tx_rdx_grade,
     "RenoDX's colour grading: the scene's contrast. 50 is neutral.", nullptr, false, true},
    {TAB_DISPLAY, "RenoDX saturation", &opts.rdx_saturation, 0, 100, 5, tx_rdx_grade,
     "RenoDX's colour grading: the scene's saturation. 50 is neutral.", nullptr, false, true},
    {TAB_DISPLAY, "RenoDX highlight saturation", &opts.rdx_highlight_sat, 0, 100, 1, tx_rdx_grade,
     "With RenoDRT: the strength of the colours of the brightest parts. 50 leaves it unchanged.", nullptr, false, true},
    {TAB_DISPLAY, "RenoDX blowout", &opts.rdx_blowout, 0, 100, 1, tx_rdx_grade,
     "With RenoDRT: turns the brightest parts towards white. 0 keeps their colour.", nullptr, false, true},
    {TAB_DISPLAY, "RenoDX flare", &opts.rdx_flare, 0, 100, 1, tx_rdx_grade,
     "With RenoDRT: lifts the darkest parts as a little stray light would. 0 keeps black black.", nullptr, false, true},
    {TAB_DISPLAY, "RenoDX hue correction", &opts.rdx_hue, 0, 100, 5, tx_percent,
     "With RenoDRT: how much bright colours keep the hue they have before the tonemapper. 0% lets them shift as the curve "
     "takes them.", nullptr, false, true},
    {TAB_DISPLAY, "RenoDX color grading", &opts.rdx_grade, 0, 100, 5, tx_percent,
     "With RenoDRT: how much of the game's own colour grading the HDR picture keeps. 0% is the scene without it.",
     nullptr, false, true},
    {TAB_DISPLAY, "RenoDX bloom", &opts.rdx_bloom, 0, 100, 5, tx_rdx_grade,
     "How strong the glow around bright things is. 50 is the game's.", nullptr, false, true},
    {TAB_GRAPHICS, "Quality", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},   // ((Triforce) 11-17: a section)
    // Graphics: the picture, top to bottom (the Upscaler only in a build with DLSS, or where AMD FSR 3.1 runs: row_shown)
    {TAB_GRAPHICS, "Quality preset", &opts.preset, 0, 5, 1, tx_preset,
     "Sets the scaling rows and memory limits for a GPU with that much memory. The Upscaler and Frame generation stay yours.",
     hn_preset},
    // (Triforce: read once, at the start (host/nn_vi.c): Auto -> 100% in game left the 3D at the dynamic path's size)
    {TAB_GRAPHICS, "Render resolution", &opts.resolution, 0, ur::kValues - 1, 1, tx_resolution,
     "The share of the output the 3D is drawn at; the Upscaler fills in the rest. Applies after a restart.", hn_resolution, true},
    // (Triforce, after NX2navi's texture_detail: tex_detail.h, vk_texture.cpp texture_detail_level. Read at the first texture)
    {TAB_GRAPHICS, "Texture detail", &opts.tex_detail, 0, 3, 1, tx_tex_detail,
     "How large the game's textures are kept in video memory. Automatic suits the graphics card; a Quality preset sets it "
     "for its card's size. Applies after a restart.", hn_tex_detail, true},
    {TAB_GRAPHICS, "Texture filtering", &opts.aniso, 0, 4, 1, tx_aniso,   // (Triforce: vk_draw.cpp sampler_for)
     "Anisotropic filtering: keeps ground, paths and walls sharp where they slope away from the camera. Game's is what the game "
     "asks for; 16x is the sharpest, at a small cost."},
    {TAB_GRAPHICS, "Upscaling and edges", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},   // ((Triforce) 11-17: a section)
    {TAB_GRAPHICS, "Upscaler", &opts.dlss, 0, ur::kUpscalers - 1, 1, tx_upscaler, "What scales the 3D up to the Output resolution.",
     hn_upscaler},
    {TAB_GRAPHICS, "DLSS model", &opts.dlss_model, 0, ur::kModels - 1, 1, tx_dlss_model,
     "Which of NVIDIA's DLSS networks draws the picture.", hn_dlss_model, false, true},
    // (Triforce: AMD's own RCAS inside the library's FSR 3.1 and FSR 4, fsr_sharpen.h; off, as before, until asked)
    {TAB_GRAPHICS, "FSR sharpening", &opts.fsr_sharpness, 0, 100, 5, tx_fsr_sharpness,
     "AMD FSR 3.1 (through AMD's library) and FSR 4: how much of FSR's own sharpening is used. Off leaves the picture as it is.",
     hn_fsr_sharpness, false, true},
    // (Triforce, 11-13: vk_temporal.cpp pre_wanted, off until asked)
    {TAB_GRAPHICS, "Upscale before bloom", &opts.upscale_pre, 0, 1, 1, tx_onoff,
     "With NVIDIA DLSS, DLAA or AMD FSR as the Upscaler: On, it works on the scene before the game's glow and tone mapping, "
     "so both are made from the sharpened scene. Off: on the finished picture, as before. Applies after a restart.",
     nullptr, true, true},
    // (Triforce, after NX2navi's aa setting: aa_rows.h, vk_aa.cpp. Taken at each frame; not a Quality preset's)
    {TAB_GRAPHICS, "Anti-aliasing", &opts.aa, 0, aa_rows::kValues - 1, 1, tx_aa,
     "How the 3D's edges are smoothed before it is scaled up: by the game's own SMAA, not at all, by FXAA, or by SMAA at "
     "its Ultra preset. Changes at once.", hn_aa, false, true},
    {TAB_GRAPHICS, "Sharpness", &opts.sharpness, 0, 100, 10, tx_percent,
     "Sharpens the picture as it goes to the window, with any filter. Too much shows halos around edges.", hn_sharpness},
    {TAB_GRAPHICS, "Window scaling filter", &opts.upscale, 0, 3, 1, tx_filter,   // (Triforce: 3 = AMD FSR 1, vk_framegen.cpp)
     "How the finished picture is scaled to the window, when its size differs from the Output resolution.", hn_filter},
    {TAB_GRAPHICS, "Smoothness", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},   // ((Triforce) 11-17: a section)
    // (Triforce: 9 is AMD FSR 4, framegen_rows.h FSR4, there only where AMD's library can make the frames: framegen_row_sync)
    {TAB_GRAPHICS, "Frame generation", &opts.framegen, 0, 9, 1, tx_framegen,
     "Makes frames between the game's own, for smoother motion, at a little input delay. Fast motion or camera cuts can "
     "show artifacts.", hn_framegen, false, true},
    // (Triforce: the engine that makes the made frames; RX_VK_FRAMEGEN_VENDOR over it. A change is taken at the next present, through the
    // engine switch (framegen_engine_tick: the swapchain made again where AMD's library comes or goes))
    {TAB_GRAPHICS, "Frame generation engine", &opts.fg_engine, 0, fgengine::kEngineRows - 1, 1, tx_fg_engine,
     "Which engine makes the made frames: Auto picks the best that runs on this PC.", hn_fg_engine, false, true},
    {TAB_GRAPHICS, "Frame pacing", &opts.pace, 0, 1, 1, tx_onoff, "When each frame is shown.", hn_pace},
    {TAB_GRAPHICS, "Latency", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},   // ((Triforce) 11-17: a section)
    {TAB_GRAPHICS, "NVIDIA Reflex", &opts.reflex, 0, 2, 1, tx_reflex,
     "NVIDIA GPUs: each frame starts as late as it can, for less input delay. With DLSS frame generation it runs through "
     "NVIDIA Streamline; not used with the other frame generation.", hn_reflex},
    {TAB_GRAPHICS, "AMD Anti-Lag 2", &opts.antilag, 0, 1, 1, tx_antilag,
     "AMD GPUs: each frame starts as late as it can, for less input delay. Not used with frame generation.", hn_antilag},
    {TAB_GRAPHICS, "Advanced", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},   // ((Triforce) 11-17: a section)
    {TAB_GRAPHICS, "Handheld profile", &opts.handheld, 0, 1, 1, tx_onoff,
     "Runs the game as if the console were undocked. Applies after a restart.", hn_handheld, true},
    {TAB_GRAPHICS, "Robust buffers", &opts.robust, 0, 2, 1, tx_robust,
     "Bounds-checked buffer reads: a read past a buffer's end is dropped instead of stopping the GPU. Applies after a "
     "restart.", hn_robust, true},
    // ((Triforce, 11-18) a player's RX 7700S: glowing doors and walls with native mesh shaders, gone with the compute fallback
    // (#bug-tracker). The row's key, mesh_shaders, is read where the device is made: vk_main.cpp)
    {TAB_GRAPHICS, "Mesh shaders", &opts.mesh_mode, 0, 1, 1, tx_mesh,
     "How the game's mesh-shader draws run. Try Compute fallback on an AMD card that shows glowing walls or loses the GPU. "
     "Applies after a restart.", hn_mesh, true},
    {TAB_SYSTEM, "Game", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},   // ((Triforce) 11-17: a section)

    // (Triforce: language.txt's choice, not the options file's. Its entries are counted when the file is read: lang_load)
    {TAB_SYSTEM, "Game language", &opts.language, 0, 0, 1, tx_language,
     "The language of the game's texts. Kept in language.txt in the game folder, which lists the languages the game has. "
     "Applies after a restart.", hn_language, true, true},
    {TAB_SYSTEM, "Game speed", &opts.realtime, 0, 1, 1, tx_speed, "How the game keeps time when it runs below 60 fps.",
     hn_speed},
    {TAB_SYSTEM, "Text speed", &opts.text_speed, 0, 3, 1, tx_text_speed,
     "How fast the letters of dialogue and other messages appear.", hn_text_speed},
    {TAB_SYSTEM, "Discord status", &opts.discord, 0, 1, 1, tx_onoff,
     "Shows \"Playing Triforce\" and the time played in your Discord status, while Discord runs on this PC.", nullptr, false,
     true},
    {TAB_SYSTEM, "Saves", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},   // ((Triforce) 11-17: a section)
    // (Triforce, after NX2navi's save rows: they act on a press, share_row; the host does the work, host/save_share.c)
    {TAB_SYSTEM, "Export save", nullptr, 0, 0, 0, nullptr,
     "Writes your save as a zip into your Downloads folder: to keep, or to give to someone. No game file is in it."},
    {TAB_SYSTEM, "Export save for a bug report", nullptr, 0, 0, 0, nullptr,
     "The same zip with the game's logs, its settings and its freeze report added: what to send with a problem report."},
    {TAB_SYSTEM, "Import a save", nullptr, 0, 0, 0, nullptr,
     "Takes a save export of Triforce or NX2navi: a zip you put into the game folder's save-import folder. It becomes your "
     "save at the next start, and the save it replaces is kept. For an emulator's save: Convert saves in Triforce Setup."},
    {TAB_SYSTEM, "Tools", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},   // ((Triforce) 11-17: a section)
    // (Triforce, after NX2navi's F8: host/bug_capture.c writes the folder, vk_bugshot.cpp its picture)
    {TAB_SYSTEM, "Bug capture (F8)", nullptr, 0, 0, 0, nullptr,
     "Writes what a problem report needs into a folder of its own under captures in the game folder: a picture of the frame, "
     "your settings, the end of the log and your save. F8 does the same while you play."},
    // (Triforce, after NX2navi's Cheats tab: the cheats of the mods' cheat files, a row each once the host has named them)
    {TAB_SYSTEM, "Mod cheats", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},   // (kModCheatHeading)
    MOD_CHEAT_ROWS8(0) MOD_CHEAT_ROWS8(8) MOD_CHEAT_ROWS8(16) MOD_CHEAT_ROWS8(24) MOD_CHEAT_ROWS8(32) MOD_CHEAT_ROWS8(40)
    MOD_CHEAT_ROWS8(48) MOD_CHEAT_ROWS8(56) MOD_CHEAT_ROWS8(64) MOD_CHEAT_ROWS8(72) MOD_CHEAT_ROWS8(80) MOD_CHEAT_ROWS8(88)
    // (Triforce, after NX2navi's volume row: over the game's own mix, host/nn_audio.c; the game's own Options have none)
    {TAB_AUDIO, "Master volume", &opts.volume, 0, 100, 5, tx_volume,
     "How loud the game is, in percent of its own volume. The game's sounds and music keep their balance.", nullptr, false,
     true},
    // (Triforce, after NX2navi's menu_sound_volume: the options' own sounds, host/menu_sound.c; under the Master volume)
    {TAB_AUDIO, "Menu sounds", &opts.menu_sounds, 0, 100, 5, tx_menu_sounds,
     "How loud these options' own sounds are: the game's interface sounds, played from your game files as you move through "
     "the tabs and rows. Off is silent. The Master volume is over them too.", nullptr, false, true},
    {TAB_AUDIO, "Audio output", &opts.audio_output, 0, 2, 1, tx_audio_output,
     "The speakers the game mixes its sound for. Applies after a restart.", hn_audio_output, true, true},
    {TAB_AUDIO, "Microphone", &opts.mic, 0, 2 + kMaxMics, 1, tx_mic,
     "What the ocarina hears when the game's microphone option is on (playing an instrument, or humming).", hn_mic, false,
     true},
    {TAB_CONTROLS, "Camera", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},   // ((Triforce) 11-17: a section)
    // (Triforce: host/prompts.c build_camera)
    {TAB_CONTROLS, "Camera speed", &opts.camera_speed, 25, 300, 5, tx_sens,
     "How fast the right stick turns the camera: times the game's own speed, at any Camera sensitivity of the game's "
     "Options. The mouse keeps its own sensitivity. Applies after a restart.", nullptr, true, true},
    {TAB_CONTROLS, "Invert camera X", &opts.invert_cam_x, 0, 1, 1, tx_onoff,
     "Inverts horizontal look (left/right) on the right analog stick.", nullptr, false, true},
    {TAB_CONTROLS, "Invert camera Y", &opts.invert_cam_y, 0, 1, 1, tx_onoff,
     "Inverts vertical look (up/down) on the right analog stick.", nullptr, false, true},
    {TAB_CONTROLS, "Mouse camera", &opts.mouse_native, 0, 1, 1, tx_mousecam, "How the captured mouse turns the camera.",
     hn_mousecam, false, true},
    {TAB_CONTROLS, "Mouse sensitivity", &opts.mouse_sens, 10, 500, 10, tx_sens,
     "How far the camera turns for a mouse movement, with either Mouse camera: at 1.00 native look turns 0.1 degree a "
     "count. The - and = keys change it in game too.", nullptr, false, true},
    {TAB_CONTROLS, "Invert mouse vertical", &opts.mouse_invert, 0, 1, 1, tx_onoff, "Swaps up and down for the mouse.",
     hn_mouse_invert, false, true},
    {TAB_CONTROLS, "Sticks", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},   // ((Triforce) 11-17: a section)
    {TAB_CONTROLS, "Stick dead zone", &opts.stick_dead_zone, 0, 5, 1, tx_stick_dead_zone,
     "How far a stick moves before the game sees it, on each axis. Lower for a precise stick, higher for one that drifts. "
     "The game's own dead zone still follows.", nullptr, false, true},
    // (Triforce, after NX2navi's lstick_deadzone and rstick_deadzone: host/nn_hid.c hos_stick_axis_own)
    {TAB_CONTROLS, "Left stick dead zone", &opts.lstick_dead_zone, -1, 50, 1, tx_own_dead_zone,
     "The left stick's own dead zone, in percent of its travel on each axis, in place of Stick dead zone: a worn stick can "
     "have a larger one than the other. 0% cuts nothing; the game's own dead zone still follows.", nullptr, false, true},
    {TAB_CONTROLS, "Right stick dead zone", &opts.rstick_dead_zone, -1, 50, 1, tx_own_dead_zone,
     "The right stick's own dead zone, in percent of its travel on each axis, in place of Stick dead zone: a worn stick can "
     "have a larger one than the other. 0% cuts nothing; the game's own dead zone still follows.", nullptr, false, true},
    {TAB_CONTROLS, "Movement", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},   // ((Triforce) 11-17: a section)
    // (Triforce, after NX2navi's sprint button: host/oot_sprint.h)
    {TAB_CONTROLS, "Sprint button", &opts.sprint_button, 0, 2, 1, tx_sprint,
     "Link sprints from a button press, without rolling first. Left stick click: the game has no other use for it. B: sprints "
     "only while Link moves, and puts the weapon away as usual otherwise (with the game's Swap Jump Controls B is the jump: "
     "use the stick's click).", nullptr, false, true},
    {TAB_CONTROLS, "Sprint mode", &opts.sprint_mode, 0, 1, 1, tx_sprint_mode,
     "Toggle: a press starts the sprint and the next press, or stopping, ends it. Hold: Link sprints while the button is "
     "held.", nullptr, false, true},
    // (Triforce, after NX2navi's walk_key and walk_scale: host/nn_hid.c walk_key_code, walk_keys)
    {TAB_CONTROLS, "Walk key", &opts.walk_key, 0, 4, 1, tx_walk_key,
     "A key that makes Link walk on the keyboard: held with the movement keys, they push the left stick only as far as "
     "Walk scale. While it is chosen the key presses nothing else.", hn_walk_key, false, true},
    {TAB_CONTROLS, "Walk scale", &opts.walk_scale, 20, 90, 1, tx_sens,
     "How far the movement keys push the left stick while the Walk key is held: 1.00 would be all the way, lower walks "
     "slower. A controller's stick and the camera's keys are not changed.", nullptr, false, true},
    {TAB_CONTROLS, "Vibration", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},   // ((Triforce) 11-17: a section)
    // (Triforce: to 300 as NX2navi's rumble_strength, 150 before)
    {TAB_CONTROLS, "Rumble strength", &opts.rumble, 0, 300, 25, tx_rumble,
     "How strongly the controller rumbles, in percent of the game's rumble. Off turns it off. Over 100% a rumble that "
     "would pass the motors' full strength stops there.", nullptr, false, true},
    // (Triforce, after NX2navi's rumble_curve: host/pad_sdl.c rumble_curve)
    {TAB_CONTROLS, "Vibration curve", &opts.rumble_curve, 20, 150, 5, tx_sens,
     "Shapes the rumble of a controller's two motors. 1.00 is the game's. Lower lifts the many weak rumbles so they can be "
     "felt and hardly changes the strong ones; higher quiets them. A Switch controller's HD rumble is not changed by it.",
     nullptr, false, true},
    // (Triforce, after NX2navi's rumble_triggers: host/pad_sdl.c trigger_poll)
    {TAB_CONTROLS, "Trigger vibration", &opts.rumble_triggers, 0, 1, 1, tx_onoff,
     "Xbox One and Series controllers: the motors in the triggers rumble too, with the lighter of the two motors. "
     "Controllers without them rumble as before.", nullptr, false, true},
    {TAB_CONTROLS, "Other", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},   // ((Triforce) 11-17: a section)

    // Controls (17 rows; Motion opens the 6 TAB_MOTION rows, Controls its page with the 2 TAB_PAGE rows)
    {TAB_CONTROLS, "Controller in background", &opts.pad_background, 0, 1, 1, tx_onoff,
     "Whether the controller plays the game while another window has the focus.", hn_background},
    {TAB_CONTROLS, "Menu shortcut", &opts.menu_shortcut, 0, 1, 1, tx_menu_shortcut,
     "Which keys open and close these options from anywhere. F1 always does; F1 only leaves Insert free for an overlay.",
     nullptr, false, true},
    {TAB_CONTROLS, "Motion", nullptr, 0, 0, 0, nullptr,   // (hookshot: opens the Motion page, the TAB_MOTION rows)
     "Motion controls: aiming with the controller's gyroscope, its sensitivity and its inverts."},
    {TAB_CONTROLS, "Controls", nullptr, 0, 0, 0, nullptr,   // (hookshot: opens its page)
     "The layout preset, the button prompts, and which gamepad button and which keys press each of the Switch's "
     "buttons."},
    {TAB_MOTION, "Motion controls", &opts.gyro, 0, 1, 1, tx_gyro,
     "Aims and looks with the controller's gyroscope where the game uses motion, as on the Switch. Works with Switch Pro "
     "controllers and pads in Switch mode, DualSense and DualShock 4. Shown beside it: whether the connected pad has one.", hn_gyro,
     false, true},
    {TAB_MOTION, "Motion sensitivity X", &opts.gyro_sens_x, 10, 600, 10, tx_percent,
     "How far the aim moves left and right for a given turn of the controller. 100% is the console's own.", nullptr, false,
     true},
    {TAB_MOTION, "Motion sensitivity Y", &opts.gyro_sens_y, 10, 300, 10, tx_percent,
     "How far the aim moves up and down for a given tilt of the controller. 100% is the console's own.", nullptr, false,
     true},
    {TAB_MOTION, "Invert motion X", &opts.gyro_inv_x, 0, 1, 1, tx_onoff, "Turns the motion aim's left and right around.",
     nullptr, false, true},
    {TAB_MOTION, "Invert motion Y", &opts.gyro_inv_y, 0, 1, 1, tx_onoff, "Turns the motion aim's up and down around.",
     nullptr, false, true},
    {TAB_MOTION, "Motion deadzone", &opts.gyro_deadzone, 0, 2, 1, tx_deadzone,
     "How slow a turn counts as none, the console's three settings: Standard is its own. High steadies a pad that jitters "
     "when held still; Low keeps the smallest movements.", nullptr, false, true},
    // (hookshot) the Controls page's own rows, above its grid (TAB_PAGE: no tab shows them)
    {TAB_PAGE, "Layout preset", &opts.pad_layout, 0, 3, 1, tx_pad_layout,
     "Which face button is which. Binding one yourself on the Controls page makes it Custom.", hn_pad_layout},
    {TAB_PAGE, "Button prompts", &opts.prompts, 0, HOS_PROMPT_N, 1, tx_prompts,
     "The button icons the game shows, in its texts and menus: those of this controller, or the keyboard's and the "
     "mouse's, for the buttons or keys your controls put each action on.", hn_prompts, true, true},
    // Debug: the tools' switch, the tools themselves below it while they are on (cheat: menu_tick does what they do), then the
    // developer's rows
    {TAB_DEBUG, "Debug tools", &opts.debug_tools, 0, 1, 1, tx_onoff,
     "Testing tools: no-clip flight, a free camera, teleport, Link's age, the inventory, the clock and heart protection. Off "
     "puts flight, the free camera and heart protection away.", nullptr, false, true},
    {TAB_DEBUG, "Heart protection", nullptr, 0, 0, 0, nullptr,
     "Protects Link's hearts. Hits can still knock him back.", nullptr, false, true, 0},
    {TAB_DEBUG, "No-clip flight", nullptr, 0, 0, 0, nullptr,
     "Enable double-tap Jump for takeoff and landing. Hold Jump to climb or L to descend.", nullptr, false, true, 1},
    {TAB_DEBUG, "Mark position", nullptr, 0, 0, 0, nullptr,
     "Remember Link's position for this loaded scene.", nullptr, false, true, 2},
    {TAB_DEBUG, "Return to mark", nullptr, 0, 0, 0, nullptr,
     "Teleport Link to the marked position in the current scene.", nullptr, false, true, 3},
    {TAB_DEBUG, "Set time", nullptr, 0, 0, 0, nullptr,
     "Choose an hour with Left or Right. Enter sets the world clock.", nullptr, false, true, 4},
    {TAB_DEBUG, "Inventory item", nullptr, 0, 0, 0, nullptr,
     "Choose a consumable with Left or Right.", nullptr, false, true, 5},
    {TAB_DEBUG, "Change quantity", nullptr, 0, 0, 0, nullptr,
     "Choose how many to add or remove. Enter applies the change.", nullptr, false, true, 6},
    {TAB_DEBUG, "Free camera", nullptr, 0, 0, 0, nullptr,
     "Move and look with your usual controls. Page Up / Page Down or R / L changes height.", nullptr, false, true, 7},
    {TAB_DEBUG, "Teleport", nullptr, 0, 0, 0, nullptr,
     "Opens the map list. Link travels through the game's own map change, in his current age.", nullptr, false, true, 8},
    {TAB_DEBUG, "Switch age", nullptr, 0, 0, 0, nullptr,
     "Swaps Link between child and adult and reloads this map in the other age (the Market when this map has no match).", nullptr, false, true, 9},
    {TAB_DEBUG, "Inventory", nullptr, 0, 0, 0, nullptr,
     "Enter opens the inventory editor: unlock or relock equipment, items, upgrades and quest items, and change amounts.", nullptr, false, true, 10},
    // (Triforce: the name pages, after NX2navi's flags by name, cutscene starter and amiibo. Nothing is read, listed or sent
    // until one is opened)
    {TAB_DEBUG, "Game flags", nullptr, 0, 0, 0, nullptr,
     "Opens the game's story and world flags by name, in groups, read from your game files. See which are set in your save, and "
     "set or clear one. The game takes a change when it runs again.", nullptr, false, true, 11},
    {TAB_DEBUG, "Cutscenes", nullptr, 0, 0, 0, nullptr,
     "Opens the game's events by name, read from your game files, and starts one as the game's own actors do. Many belong to "
     "one place or one moment of the story and may not play as they should anywhere else. Save first.", nullptr, false, true, 12},
    {TAB_DEBUG, "Scan amiibo", nullptr, 0, 0, 0, nullptr,
     "Opens the dumps in the folder \"amiibo\" in the game folder (.bin files you made of your own figures). Choose one, then use "
     "amiibo in the game: the figure is on the reader when the game looks for one.", nullptr, false, true, 13},
    {TAB_DEBUG, "Developer", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},
    {TAB_DEBUG, "Descriptor reuse", &opts.descriptor_reuse, 0, 1, 1, tx_onoff,
     "A draw keeps its descriptors while the texture slots it samples are unchanged, so streaming elsewhere in the game no longer "
     "rebuilds every set each frame. Applies after restart.", nullptr, true},
    // (Triforce: vk_draw.cpp gpl_wanted, what RX_VK_GPL=1 does; gpl_rules.h. On by default since options_version 5)
    {TAB_DEBUG, "Fast pipeline link", &opts.fast_pipeline_link, 0, 1, 1, tx_onoff,
     "Experimental. A new pipeline is first linked from its shaders' libraries, in milliseconds where the graphics driver has "
     "them compiled, and the optimised one takes its place once it is built. The first start with it compiles those libraries "
     "once, which takes minutes. Needs a card with pipeline libraries. Applies after restart.", nullptr, true, true},
    {TAB_DEBUG, "Sampler cache", &opts.sampler_cache, 0, 1, 1, tx_onoff,
     "What a sampler holds is kept beside it on the replay thread, so resolving a sampler that was not registered anew takes no "
     "lock. Helps while the game streams textures. Applies after restart.", nullptr, true},
    // (Triforce: vk_memory.cpp "lazy write-back", wb_lazy_state.h; RX_VK_LAZY_WB over it. On by default since options_version
    // 5, but nowhere on AMD's driver by this row alone. Windows only: row_shown)
    {TAB_DEBUG, "Lazy write-back", &opts.lazy_write_back, 0, 1, 1, tx_onoff,
     "Experimental. What the GPU writes into memory the game shares with it is copied back only when the game, the system or "
     "the GPU next reads it there, not after every submission. Saves memory traffic where the game reads little of it. "
     "Applies after restart.", hn_lazy_write_back, true, true},
    // (Triforce: vk_replay.cpp par_threads_max, what RX_VK_PARALLEL_AUTO=<n> does. Automatic unless set here: replay_threads.h)
    {TAB_DEBUG, "Replay helper threads", &opts.replay_threads, 0, 8, 1, tx_replay_threads,
     "A frame's large command buffers are recorded on this many threads wherever short trials, made every so often as the game "
     "runs, find that faster than the replay thread alone. Automatic: 2 or 3 threads by this PC's processors, none with four "
     "cores or fewer. Off: the replay thread alone, always: choose it if frames flicker. Used with DLSS or FSR on too, for "
     "upscaling or frame generation: what comes after the upscale is recorded in order. Applies after restart.",
     hn_replay_threads, true, true},
    // (Triforce, after NX2navi's tail_early: vk_replay.cpp the early tail, tail_early.h. Read at the start. The owner, 11-16:
    // "implement this in debug under developer" and "on by default": a Developer row, on by default since options_version 6)
    {TAB_DEBUG, "Start next frame early", &opts.tail_early, 0, 1, 1, tx_onoff,
     "Experimental. The game starts its next frame while the graphics card still draws the end of this one (the upscaling "
     "and the interface), where that end holds nothing the game reads back. Other frames run as before. Applies after "
     "restart.", nullptr, true, true},
    // (Triforce, after NX2navi's astc_bc1: tex_bc1.h, vk_texture.cpp bc1_on. Read at the first texture; on by default since
    // options_version 5. The owner, 11-16: "put this in debug dev tab too": a Developer row, after Start next frame early)
    {TAB_DEBUG, "Opaque textures as BC1", &opts.tex_bc1, 0, 1, 1, tx_onoff,
     "Keeps the game's large textures that have no transparency in half the video memory, with the same picture. Off: as "
     "before. Turn it off if textures look wrong. Applies after a restart.",
     hn_tex_bc1, true, true},
    {TAB_SYSTEM, "Exit", nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, true, -1, true},   // ((Triforce) 11-17: a section)
    // (Restart and Quit stay the table's last two: kRestartRow and kQuitRow count back from its end)
    {TAB_SYSTEM, "Restart", nullptr, 0, 0, 0, nullptr,
     "Starts the game again. Save your progress first. Press Enter or A twice to restart. Esc or B "
     "cancels."},
    {TAB_SYSTEM, "Quit", nullptr, 0, 0, 0, nullptr,
     "Save your progress first. Press Enter or A twice to quit. Esc or B cancels."},

};
constexpr int kQuitRow = sizeof rows / sizeof rows[0] - 1;
constexpr int kRestartRow = kQuitRow - 1;
const Row* pending = nullptr;   // (Restart or Quit: shows Confirm until its second press)
bool cheats_env = false;   // (HOS_OOT_CHEATS=1: on whatever the Debug tab says)
/// The debug tools: the Debug tab's row, or HOS_OOT_CHEATS=1
bool cheats_on() { return cheats_env || opts.debug_tools; }
// (Triforce) The Debug tab: shown always since 11-15 (the owner: "make debug on by default and remove the alt+g shortcut").
// Alt+G, which unlocked it and put it away again, is gone; the options file's old debug_tab= line is dropped at the next save
bool debug_shown() { return true; }
int tabs_shown() { return debug_shown() ? TAB_COUNT : TAB_COUNT - 1; }
// (Triforce) Inside the game's own Options screen. host/oot_menu.c says each frame that screen runs (menu_option_set,
// HOS_OPT_GAME_OPTIONS). While it does, the options' tabs are drawn over its header with one more in front, "Game": the
// game's own list, which stays the game's (drawn by it, its input its own). L and R (the buttons the Switch's L and R are
// bound to, or their keys: Q and E are the game's own keys there) go on to our tabs and back; on ours the game's list,
// caption and guides are under the same plates, ours, which are opaque. Back on one of ours is the game's Back too
// (host/nn_hid.c). TRIFORCE_OPTIONS_TABS=0: off (the game's Options as it is, ours through F1 alone)
std::atomic<int64_t> game_options_ns{0};   // (prof_now at the last frame the host said so; 0 never)
bool embed_open = false;                   // (our tabs are open from inside the game's Options: leaving them is its own tab again)
int64_t embed_since = 0;                   // (when that screen was first seen running: the tabs fade in over its opening)
constexpr int kGameTab = 99;               // (the tabs' hit for the game's own tab)
// ((Triforce) the mouse over the tabs on the game's own tab (draw_game_tab): their shift in the window, as menu_ox is draw_menu's,
// and whether the mouse has been read since the game's screen came: a click from before is not the tabs')
float game_tab_ox = 0;
bool game_tab_read = false;
bool hits_game_tab = false;   // (the hits are draw_game_tab's: its tabs were drawn last, not the open menu)
// (Triforce) Mods: an entry of the tab strip after System (before Debug) that opens a page of its own, kModsPage (vk_mods.h:
// GameBanana's mods for this game, installed into the mods folder). Not a MenuTab: it has no rows
constexpr int kModsTab = 98;               // (the tabs' hit for it, and draw_tabs' sel while its page is open)
constexpr int kModsPage = 8;               // (page: 0 the options, 1 controls, 3 teleport, 4 inventory, 5 the restart question, 6 names)
int mods_choice = 0;                       // (the card chosen)
float mods_scroll_px = 0, mods_want_px = 0;
int mods_wheel = 0, mods_page_rows = 1;
// ((Triforce) 11-17, his "theres not even a way to scroll down on the mods description on the right side") The description's
// own scroll: the right stick, or the mouse wheel over the details; back to its top when the choice moves
int mods_desc_wheel = 0, mods_desc_for = -2;
// ((Triforce) 11-17, his layout A) The Mods screen's categories, a strip of tabs under the banner turned by L and R: the loaded
// list filtered by GameBanana's root category (or installed). A mod whose category none names is in All alone
constexpr int kModsCats = 6;
const char* const kModsCatNames[kModsCats] = {"All", "Gameplay", "Models", "Audio", "Interface", "Installed"};
int mods_cat = 0;
int mods_cat_pick = -1;   // (a click on a category's tab: taken by the next tick)
bool mods_in_cat(const mods::Mod& m, int cat) {
    auto has = [&m](const char* w) { return m.category.find(w) != std::string::npos; };
    switch (cat) {
    case 1: return has("Gameplay") || has("Mechanic") || has("Cheat");
    case 2: return has("Model") || has("Texture") || has("Skin") || has("Character");
    case 3: return has("Audio") || has("Sound") || has("Music") || has("Voice");
    case 4: return has("UI") || has("HUD") || has("Interface") || has("Menu") || has("Font");
    case 5: return m.installed;
    default: return true;
    }
}
float mods_desc_px = 0, mods_desc_x0 = 1e9f;
uint32_t menu_pad_raw = 0;   // (the pads' bits at this tick, host/nn_hid.c hos_pad_raw: the sticks' menu bits too)
bool mods_follow = true;                   // (the cards follow the choice; the wheel lets go of it until the choice moves)
int mods_btn = 0;                          // (a dialog's button chosen: 0 the upper one)
// (Triforce) The Mods entry under a game list's last button (host/oot_menu.c tells where: HOS_OPT_MODS_*): the pause menu's
// System page and the title's list. While the cursor is on it the menu is "open" on kModsRowPage, which draws the entry alone
// over the game's list (the game's input is ours then, and the host lets its cursor go); A opens kModsPage
constexpr int kModsRowPage = 9;
std::atomic<int> mods_list_bits{0};        // (HOS_MODS_LIST_*, as the last report had them)
std::atomic<int> mods_box[5];              // (the last button's box and the pitch, the layout's units: x0 x1 y0 y1 pitch)
std::atomic<int64_t> mods_list_ns{0};      // (prof_now at the last report)
std::atomic<int> mods_enter{0};            // (the host's word: 1 the cursor onto the entry, 2 a click on it)
std::atomic<int> mods_press{0};            // (presses the host passed on while Mods has the cursor: HOS_MODS_PRESS_*)
bool mods_list_fresh() {
    const int64_t t = mods_list_ns.load(std::memory_order_relaxed);
    return t && prof_now() - t < 150000000LL;
}
// ((Triforce) 11-17, his ask "We need a search bar for mods") The Mods screen's search: a bar above the cards (mods_choice -1 is
// the bar), typed into from the keyboard anywhere on the screen (host/display.c: HOS_OPT_TEXT_CHAR), or from a keyboard of the
// game's look on the screen for a controller (A on the bar). The text is searched 0.5 s after the typing stops, or at once
// with Confirm; fewer than 2 characters is the whole list again, where it was (vk_mods.cpp search)
std::string mods_query;              // (the bar's text as typed, UTF-8)
std::string mods_query_sent;         // (the text the list is the search of)
int64_t mods_query_edit_ns = 0;      // (the last change of the text)
bool mods_kbd = false;               // (the on-screen keyboard open)
int mods_kbd_row = 1, mods_kbd_col = 0;
int mods_browse_choice = 0;          // (the whole list's choice and scroll when a search began: given back when it ends)
float mods_browse_px = 0;
std::mutex mods_typed_mu;
std::vector<int> mods_typed;         // (code points typed, from the window's thread: taken by mods_tick)
std::atomic<bool> mods_text_wanted{false};
float mods_kbd_box[4] = {0, 0, 0, 0};   // (the on-screen keyboard's box as last drawn, pixels: x0 y0 x1 y1)
// (the keyboard: four rows of ten keys, the letters shown capital and typed small, and a row of Space, Delete, Clear, Search)
const char* const kKbdRows[4] = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm-.&"};
const char* const kKbdWide[4] = {"Space", "Delete", "Clear", "Search"};
constexpr int kKbdWideAt[5] = {0, 4, 6, 8, 10};   // (the wide keys' first columns, and the row's end)
int kbd_wide(int col) { return col < 4 ? 0 : col < 6 ? 1 : col < 8 ? 2 : 3; }
void utf8_add(std::string& s, int cp) {
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) s += (char)(0xC0 | cp >> 6), s += (char)(0x80 | (cp & 63));
    else if (cp < 0x10000) s += (char)(0xE0 | cp >> 12), s += (char)(0x80 | (cp >> 6 & 63)), s += (char)(0x80 | (cp & 63));
    else s += (char)(0xF0 | cp >> 18), s += (char)(0x80 | (cp >> 12 & 63)), s += (char)(0x80 | (cp >> 6 & 63)), s += (char)(0x80 | (cp & 63));
}
bool utf8_pop(std::string& s) {   // (the last character, all its bytes)
    if (s.empty()) return false;
    while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();
    if (!s.empty()) s.pop_back();
    return true;
}
bool embed_allowed() {
    static const bool on = !getenv("TRIFORCE_OPTIONS_TABS") || atoi(getenv("TRIFORCE_OPTIONS_TABS")) != 0;
    return on;
}
/// (Triforce, 11-13, the user: "Remove f1 menu (we have it in options now natively)") Whether Insert, F1, a gamepad's Home and
/// Select+Start open these options over the game from anywhere. Not any more: they are the tabs inside the game's own
/// Options screen. TRIFORCE_INGAME_MENU=1 (triforce_env.txt) brings the keys back; with TRIFORCE_OPTIONS_TABS=0 (no tabs in
/// the game's screen) they stay, or there would be no way to the options at all
bool overlay_allowed() {
    static const bool on = (getenv("TRIFORCE_INGAME_MENU") && atoi(getenv("TRIFORCE_INGAME_MENU")) != 0) || !embed_allowed();
    return on;
}
/// The game's Options screen ran within the last quarter second
bool game_options_shown() {
    const int64_t t = game_options_ns.load(std::memory_order_relaxed);
    return embed_allowed() && t && prof_now() - t < 250000000LL;
}
// (a line shown for a moment at the foot of the screen: the Debug tab unlocked or locked while the options are closed)
char notice_text[64] = "";
int64_t notice_until = 0;
std::atomic<int> protect_hearts{-1};
// -1 disables the shortcut, 0 enables it on the ground, and 1 means flying.
std::atomic<int> player_flight{-1};
std::atomic<int> free_camera{0};
std::atomic<int> cheat_action{0};
std::atomic<int> link_age{-1};   // (the game side's report, OOT_CHEAT_AGE_ID: 0 child, 1 adult, -1 no save loaded)
int clock_hour = 12;
int pouch_item = 0;
int pouch_delta = 1;
constexpr const char* pouch_items[] = {"Deku Seeds", "Deku Nuts", "Deku Sticks", "Bombs", "Arrows"};
constexpr int kPouchItems = sizeof pouch_items / sizeof pouch_items[0];
// (teleport list: generated by work/oot-teleport-tools/write_destinations.py)
// (the order of oot_destinations in host/hos_main.c; Enter sends 1000 + the index)
constexpr const char* teleport_names[] = {
    "Kokiri Forest",
    "Hyrule Field",
    "Kakariko Village",
    "Market",
    "Hyrule Castle",
    "Lost Woods",
    "Sacred Forest Meadow",
    "Lake Hylia",
    "Graveyard",
    "Death Mountain Trail",
    "Death Mountain Crater",
    "Goron City",
    "Zora's River",
    "Zora's Domain",
    "Zora's Fountain",
    "Gerudo Valley",
    "Gerudo Fortress",
    "Haunted Wasteland",
    "Desert Colossus",
    "Inside the Deku Tree",
    "Dodongo's Cavern",
    "Jabu-Jabu's Belly",
    "Forest Temple",
    "Fire Temple",
    "Water Temple",
    "Shadow Temple",
    "Spirit Temple",
    "Bottom of the Well",
    "Ice Cavern",
    "Gerudo Training Ground",
    "Ganon's Castle"};
constexpr const char* teleport_notes[] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "adult only", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
struct TeleportSection { const char* name; int first; };
constexpr TeleportSection teleport_sections[] = {{"Overworld", 0}, {"Dungeons", 19}};
constexpr int kTeleportSections = sizeof teleport_sections / sizeof teleport_sections[0];
// (end of the generated teleport list)
constexpr int kTeleports = sizeof teleport_names / sizeof teleport_names[0];
int teleport_choice = 0;
int teleport_columns = 1, teleport_page_rows = 1;   // (as last drawn: the cards in a row, the rows that fit: Page Up / Down)
float teleport_scroll_px = 0, teleport_want_px = 0;   // (the cards' scroll, eased toward teleport_want_px)
bool teleport_follow = true;   // (the scroll follows the choice; the wheel lets go of it until the choice moves)
int teleport_wheel = 0;        // (the wheel's notches since the last drawing: it scrolls by them)
int teleport_section_of(int i) {
    int section = 0;
    while (section + 1 < kTeleportSections && i >= teleport_sections[section + 1].first) ++section;
    return section;
}
int teleport_section_end(int section) {
    return section + 1 < kTeleportSections ? teleport_sections[section + 1].first : kTeleports;
}
/// The destination rows rows down (up when negative) from i, in the grid the Teleport page draws: each section's cards
/// teleport_columns to a row, under the section's heading; the column kept where the next row has it
int teleport_step_rows(int i, int rows) {
    const int cols = std::max(1, teleport_columns);
    for (; rows > 0; --rows) {
        const int section = teleport_section_of(i), first = teleport_sections[section].first, end = teleport_section_end(section);
        const int p = i - first;
        if (p + cols < end - first) i += cols;
        else if (p / cols < (end - first - 1) / cols) i = end - 1;   // (the section's last row, a short one)
        else if (section + 1 < kTeleportSections) i = std::min(end + p % cols, teleport_section_end(section + 1) - 1);
        else break;
    }
    for (; rows < 0; ++rows) {
        const int section = teleport_section_of(i), first = teleport_sections[section].first, p = i - first;
        if (p >= cols) i -= cols;
        else if (section > 0) {
            const int prev = teleport_sections[section - 1].first, n = first - prev;
            i = std::min(prev + (n - 1) / cols * cols + p, first - 1);
        } else break;
    }
    return i;
}
// the Inventory page (gpu/vk/oot_cheat_items.h): items and their section headings, counts from the game side
#define OOT_MENU_ITEM_LABEL(label, name, kind) label,
#define OOT_MENU_ITEM_KIND(label, name, kind) kind,
#define OOT_MENU_NO_SECTION(name)
constexpr const char* item_names[] = {OOT_CHEAT_ITEMS(OOT_MENU_NO_SECTION, OOT_MENU_ITEM_LABEL)};
constexpr unsigned char item_kinds[] = {OOT_CHEAT_ITEMS(OOT_MENU_NO_SECTION, OOT_MENU_ITEM_KIND)};
constexpr int kItems = sizeof item_names / sizeof item_names[0];
// (each section's first item: a counter that a section heading reads and an item bumps)
constexpr int item_section_count() {
    int n = 0;
#define OOT_MENU_COUNT_SECTION(title) n++;
#define OOT_MENU_SKIP_ITEM(label, name, kind)
    OOT_CHEAT_ITEMS(OOT_MENU_COUNT_SECTION, OOT_MENU_SKIP_ITEM)
    return n;
}
constexpr int kItemSections = item_section_count();
struct ItemSections { TeleportSection s[kItemSections]; };
constexpr ItemSections make_item_sections() {
    ItemSections r{};
    int section = 0, item = 0;
#define OOT_MENU_ADD_SECTION(title) r.s[section].name = title, r.s[section].first = item, section++;
#define OOT_MENU_ADD_ITEM(label, name, kind) item++;
    OOT_CHEAT_ITEMS(OOT_MENU_ADD_SECTION, OOT_MENU_ADD_ITEM)
    return r;
}
constexpr ItemSections item_sections = make_item_sections();
std::atomic<int> item_counts[kItems];   // (-1 until the game side reports: OOT_CHEAT_ITEM_BASE + i)
int item_choice = 0;
int inventory_columns = 1, inventory_capacity = 1;   // (as last drawn: the cards in a row, on a page)
int inventory_filter = 0;   // (All, then the item sections)
int inventory_page = 0;     // (the page shown: the choice's, or another the wheel turned to)
bool inventory_amount = false;   // (Left and Right change the chosen item's amount)
int inventory_begin() { return inventory_filter ? item_sections.s[inventory_filter - 1].first : 0; }
int inventory_end() { return inventory_filter && inventory_filter < kItemSections ? item_sections.s[inventory_filter].first : kItems; }
void inventory_reveal() { inventory_page = (item_choice - inventory_begin()) / std::max(1, inventory_capacity); }
void choose_inventory_filter(int filter) {
    inventory_filter = filter;
    item_choice = inventory_begin();
    inventory_amount = false;
    inventory_reveal();
}
constexpr int kCheatRows = 14;   // (the debug tools' rows: Row::cheat 0 to 13; 11 to 13 Triforce's name pages)
/// A debug tool's value as its row shows it ("" an action: confirm does it)
std::string cheat_value(int i) {
    char t[16];
    switch (i) {
    case 0: return tx_onoff(protect_hearts.load() == 1);
    case 1: return tx_onoff(player_flight.load() >= 0);
    case 4: return snprintf(t, sizeof t, "%02d:00", clock_hour), t;
    case 5: return pouch_items[pouch_item];
    case 6: return snprintf(t, sizeof t, "%+d", pouch_delta), t;
    case 7: return tx_onoff(free_camera.load());
    default: return "";
    }
}
/// The tools that choose with Left and Right (arrows at their value's ends): Set time, Inventory item, Change quantity
bool cheat_chooses(int i) { return i == 4 || i == 5 || i == 6; }
/// Switch age as Kid | Adult, Link's age lit: once the game side has said it (a save loaded)
bool age_shown(int i) { return i == 9 && link_age.load(std::memory_order_relaxed) >= 0; }
/// What confirm does on a tool's row, for its prompt (nullptr: nothing)
const char* cheat_confirm(int i) {
    static const char* const says[kCheatRows] = {"Toggle", "Toggle", "Apply", "Apply", "Set", nullptr, "Apply", "Toggle", "Open list",
                                                 "Switch", "Open", "Open list", "Open list", "Open list"};
    return i >= 0 && i < kCheatRows ? says[i] : nullptr;
}
constexpr int kRows = sizeof rows / sizeof rows[0];

int cur_tab = TAB_DISPLAY;
int tab_selected[TAB_COUNT + 1] = {};   // (and the Motion page's)
bool is_motion_row(const Row& r) { return r.tab == TAB_CONTROLS && !r.value && !strcmp(r.name, "Motion"); }

// (Triforce) The System tab's rows that act on a press, after NX2navi's: Export save, Export save for a bug report, Import a
// save and Bug capture. The work is the host's, on a thread of its own (host/save_share.c, host/bug_capture.c): a press leaves
// what was asked for it to take (HOS_OPT_SHARE_ASK), and it sends back the line the row shows under its help
// (HOS_OPT_SHARE_TEXT, HOS_OPT_SHARE_SAID).
/// Which of them a row is: its HOS_SHARE_* (vk_api.h), what the host is asked and its place in share_lines; 0: none
int share_row(const Row& r) {
    if (r.tab != TAB_SYSTEM || r.value || r.cheat >= 0) return 0;
    if (!strcmp(r.name, "Export save")) return HOS_SHARE_EXPORT;
    if (!strcmp(r.name, "Export save for a bug report")) return HOS_SHARE_EXPORT_BUG;
    if (!strcmp(r.name, "Import a save")) return HOS_SHARE_IMPORT;
    if (!strcmp(r.name, "Bug capture (F8)")) return HOS_SHARE_CAPTURE;
    return 0;
}
std::atomic<int> share_ask{0};              // (what a row asked for, until the host takes it)
std::atomic<int> share_import_waits{0};     // (an imported save waits for the next start: the Import row cancels it)
std::atomic<int64_t> share_busy_until{0};   // (a press waits for the host's answer until then: no second one meanwhile)
std::atomic<int> share_busy_row{0};         // (the row that press was on: its line is the answer)
std::mutex share_mu;                        // (the lines: the host's thread writes them, the drawing reads them)
std::string share_in;                       // (the line the host is sending, a byte a call)
std::string share_lines[HOS_SHARE_NOTICE + 1];   // (by HOS_SHARE_*: what each row shows under its help; the notice's until shown)
std::atomic<bool> share_notice_new{false};   // (a line for the foot of the screen came: menu_tick shows it)
constexpr const char* kShareWorking = "Working...";
/// What a share row shows in its value's place and in the guides: what a press does
const char* share_label(int row) {
    if (row == HOS_SHARE_CAPTURE) return "Capture";
    if (row != HOS_SHARE_IMPORT) return "Export";
    return share_import_waits.load(std::memory_order_relaxed) ? "Cancel the import" : "Import";
}
/// The line under a share row's help: the host's last word on it
std::string share_line(int row) {
    std::lock_guard lk(share_mu);
    if (share_lines[row] == kShareWorking && prof_now() > share_busy_until.load(std::memory_order_relaxed))
        return "The game did not answer. Try again in a moment.";
    return share_lines[row];
}
/// A press on a share row: asked of the host, once until it answers (or ten seconds went by)
void share_press(int row) {
    const int64_t now = prof_now();
    if (now < share_busy_until.load(std::memory_order_relaxed)) return;
    share_busy_until.store(now + 10000000000LL, std::memory_order_relaxed);
    share_busy_row.store(row, std::memory_order_relaxed);
    {
        std::lock_guard lk(share_mu);
        share_lines[row] = kShareWorking;
    }
    const bool cancel = row == HOS_SHARE_IMPORT && share_import_waits.load(std::memory_order_relaxed);
    share_ask.store(cancel ? HOS_SHARE_IMPORT_CANCEL : row, std::memory_order_relaxed);
}

// ---- the rows that take a restart: their values at the start (menu_init, the GPU's at menu_set_gpu_count) ----
namespace mr = menu_restart;
mr::State restart_state;
int restart_slot[kRows];             // (a row's slot in restart_state, -1 none)
const char* restart_names[mr::kMax];
void restart_init() {
    static bool done = false;
    if (done) return;
    done = true;
    for (int i = 0; i < kRows; i++) {
        restart_slot[i] = rows[i].restart && rows[i].value ? restart_state.add(*rows[i].value) : -1;
        if (restart_slot[i] >= 0) restart_names[restart_slot[i]] = rows[i].name;
    }
}
int restart_slot_of(const Row& r) { return restart_slot[&r - rows]; }
/// A row that takes a restart, changed from what the game started with (a row past menu_restart.h's kMax has no slot: never)
bool restart_changed(const Row& r) {
    const int slot = restart_slot_of(r);
    return r.value && r.restart && slot >= 0 && *r.value != restart_state.start[slot];
}
void restart_values(int* now) {
    for (int i = 0; i < kRows; i++)
        if (restart_slot[i] >= 0) now[restart_slot[i]] = *rows[i].value;
}
/// The rows whose value is not the one the game runs with (a bit a slot)
unsigned restart_pending() {
    int now[mr::kMax];
    restart_values(now);
    return restart_state.pending(now);
}
/// Whether closing the menu now asks to restart: something pending, not declined with Later, and a window to close
bool restart_ask() {
    int now[mr::kMax];
    restart_values(now);
    return app_window() && restart_state.ask(now);
}
std::string restart_list(const char* last) {
    return mr::names(restart_names, restart_state.n, restart_pending(), ", ", last);
}
int ask_sel = 0;   // (the restart question's choice: 0 Later, 1 Restart now)

/// A row on its tab: the debug tools only with them on, the Upscaler only in a build with DLSS or FSR 4, or where AMD
/// FSR 3.1 runs
bool rdx_row(const int* v) {   // (local, RenoDX port)
    return v == &opts.rdx_tonemap || v == &opts.rdx_midgray || v == &opts.rdx_ui_white || v == &opts.rdx_space || v == &opts.rdx_exposure ||
           v == &opts.rdx_highlights || v == &opts.rdx_shadows || v == &opts.rdx_contrast || v == &opts.rdx_saturation ||
           v == &opts.rdx_bloom ||
           // (Triforce: NX2navi's HDR rows)
           v == &opts.rdx_gamma || v == &opts.rdx_match_slope || v == &opts.rdx_tone_slope || v == &opts.rdx_hue_clip ||
           v == &opts.rdx_highlight_sat || v == &opts.rdx_blowout || v == &opts.rdx_flare || v == &opts.rdx_hue || v == &opts.rdx_grade;
}
/// (Triforce) A RenoDX row applies to the tonemapper chosen: the rows only PsychoV35 reads show with PsychoV35 (1), those
/// only RenoDRT reads with RenoDRT (2); every other row whatever is chosen, as before
bool rdx_applies(const int* v) {
    if (v == &opts.rdx_match_slope || v == &opts.rdx_tone_slope || v == &opts.rdx_hue_clip) return opts.rdx_tonemap == 1;
    if (v == &opts.rdx_highlight_sat || v == &opts.rdx_blowout || v == &opts.rdx_flare || v == &opts.rdx_hue || v == &opts.rdx_grade)
        return opts.rdx_tonemap == 2;
    return true;
}
/// (Triforce) The Frame generation row ends at AMD FSR 4 only where AMD's library can make the frames (vk_fsr4.h fsr4fg,
/// framegen_rows.h row_hi), and at 8x elsewhere, the row as it was. Asked when the menu opens, as the Upscaler's choices are.
/// The table's own end is AMD FSR 4, so an options file with it loads as it is, before the graphics card is known; a saved
/// AMD FSR 4 that cannot run here stays saved, shows as 2x (tx_framegen) and the arrows leave it for 8x or Off. (Triforce) As
/// known: the library is asked off the menu's thread (menu_ahead), and the row is set again once it has answered
void framegen_row_sync() {
    static_assert(framegen_rows::kRowMax == 9, "the Frame generation row's entry in rows[] ends at 9");
    for (Row& r : rows)
        if (r.value == &opts.framegen) r.hi = framegen_rows::row_hi(fsr4fg::offered_known());
}
// (Triforce) What the rows ask of NVIDIA's and AMD's libraries: whether DLSS, AMD FSR 4 and FSR 4's frame generation run
// here. Asked at the menu's open on its thread (the presenting one), NGX's start and AMD's loader held the first open of
// the options 2.1 s (test11). Now asked once, on a thread of its own, once the game's first frame is up (menu_tick), or at an open
// before that; until the answers are in, the rows show what is known (DLSS and FSR 4 as offered, AMD FSR 4's frame
// generation as not known yet), and are set again when they come. A choice of DLSS or FSR 4 before then starts it where it
// runs, as before (its start waits for the answer there). The probes are the libraries' own (dlss_probe, fsr4_probe,
// fsr4fg::why): once each, under their own locks. TRIFORCE_MENU_AHEAD=0: asked at the open on the menu's thread, as before
std::atomic<int> ahead_state{0};   // (0 not asked, 1 being asked, 2 answered)
bool ahead_allowed() {
    static const bool on = !getenv("TRIFORCE_MENU_AHEAD") || atoi(getenv("TRIFORCE_MENU_AHEAD")) != 0;
    return on;
}
void ahead_thread() {
    const int64_t t0 = prof_now();
    dlss_probe();
    const int64_t t1 = prof_now();
    fsr4_probe();
    const int64_t t2 = prof_now();
    (void)fsr4fg::why();
    const int64_t t3 = prof_now();
    logf("menu: the upscalers asked ahead of the menu, on a thread of their own: NVIDIA DLSS %.0f ms, AMD FSR 4 %.0f ms, its frame "
         "generation %.0f ms", (t1 - t0) / 1e6, (t2 - t1) / 1e6, (t3 - t2) / 1e6);
    ahead_state.store(2, std::memory_order_release);
}
void menu_ahead() {
    if (!dev.device) return;
    if (!ahead_allowed()) {   // (as before: on this thread, at the open)
        dlss_probe(), fsr4_probe(), (void)fsr4fg::why();
        ahead_state.store(2, std::memory_order_release);
        return;
    }
    int none = 0;
    if (ahead_state.compare_exchange_strong(none, 1)) std::thread(ahead_thread).detach();
}
bool heading_over_shown(int i);
bool row_shown(int i) {
    return (rows[i].cheat < 0 || cheats_on()) &&
           // ((Triforce) 11-17, the tabs in sections: a heading only over a row shown, RenoDX's with no RenoDX build not left bare)
           (!rows[i].heading || heading_over_shown(i)) &&
           (rows[i].value != &opts.dlss || ur::upscaler_shown(dlss_built() || fsr4_built(), fsr3::supported() || ffx3_usable())) &&
           (rows[i].value != &opts.dlss_model || ur::model_shown(dlss_built())) &&
           (rows[i].value != &opts.fsr_sharpness || ur::fsr_sharpen_shown(fsr4_built(), ffx3_usable())) &&
           // (local, RenoDX port: its rows with HDR Native, in a build with its shaders; Triforce: and its own row on, and
           // of the rows one tonemapper alone reads those of the one chosen)
           (!rdx_row(rows[i].value) || (opts.hdr == 2 && renodx_built() && opts.renodx && rdx_applies(rows[i].value))) &&
           (rows[i].value != &opts.renodx || renodx_built()) &&
#ifndef _WIN32   // (Triforce: the lazy write-back's fault handler pays on Windows only, vk_memory.cpp)
           rows[i].value != &opts.lazy_write_back &&
#endif
           // (Triforce, 11-13: the Menu shortcut row chooses the keys that open the options from anywhere: gone with them)
           (rows[i].value != &opts.menu_shortcut || overlay_allowed()) &&
           // (Triforce: the Game language row only with a language.txt that names languages to choose from)
           (rows[i].value != &opts.language || lang_count > 1) &&
           // (Triforce: the mods' cheats: as many rows as the host named, and their heading with them)
           (!rows[i].heading || strcmp(rows[i].name, kModCheatHeading) || mod_cheats.shown() > 0) &&
           (mod_cheat_slot(rows[i].name) < 0 || mod_cheat_slot(rows[i].name) < mod_cheats.shown());
}
bool heading_over_shown(int i) {   // (the rows of its tab after it, to the next heading: one of them shown)
    for (int j = i + 1; j < kRows; j++) {
        if (rows[j].tab != rows[i].tab) continue;
        if (rows[j].heading) return false;
        if (row_shown(j)) return true;
    }
    return false;
}
int tab_row_count(int t) {
    int c = 0;
    for (int i = 0; i < kRows; i++) if (rows[i].tab == (MenuTab)t && row_shown(i)) c++;
    return c;
}

int tab_row_to_global(int t, int local_idx) {
    int count = 0;
    for (int i = 0; i < kRows; i++) {
        if (rows[i].tab == (MenuTab)t && row_shown(i)) {
            if (count == local_idx) return i;
            count++;
        }
    }
    return 0;
}

std::atomic<bool> open_now{false};
bool menu_refresh = false;
// (a value too long for its row, a GPU's name: the selected row's slides through it, a character a step, resting at
// each end; the other rows end it with "...". The drawing (fit) says how far it can slide, menu_tick moves it)
int marquee_pos = 0, marquee_max = 0;
int64_t marquee_next = 0;
constexpr int64_t kMarqueeStep = 150000000LL, kMarqueeRest = 1200000000LL;   // (ns)
// (hookshot) the mouse (menu_tick): the tab strip's part under the cursor (a tab 0 to TAB_COUNT - 1, TAB_COUNT the previous,
// TAB_COUNT + 1 the next; -1 none); the slider held with the left button (its row in the tab, -1 none)
int hover_tab = -1;
int drag_row = -1;
bool mouse_read = false;   // (the menu has read the mouse since it opened)
bool is_slider(const Row& r) { return r.value == &opts.sharpness || r.value == &opts.mouse_sens || r.value == &opts.gyro_sens_x || r.value == &opts.gyro_sens_y; }
/// (a scale: the arrows and a click stop at its ends; Rumble strength would go from Off to its strongest)
bool stops_at_ends(const Row& r) { return r.value == &opts.mouse_sens || r.value == &opts.rumble || r.value == &opts.rumble_curve || r.value == &opts.lstick_dead_zone || r.value == &opts.rstick_dead_zone || r.value == &opts.walk_scale || r.value == &opts.gyro_sens_x || r.value == &opts.gyro_sens_y; }
/// (the ini's Rumble strength on the menu's steps: 0 to 300, the nearest 25)
int rumble_step(int v) { return (std::min(std::max(v, 0), 300) + 12) / 25 * 25; }
std::string ini_path() {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    std::string dir(exe, n);
    return dir.substr(0, dir.find_last_of("\\/") + 1) + "triforce_options.ini";
}
const char* ini_key(const Row& r) {
    if (r.value == &opts.output_res) return "output_res";   // (host/nn_applet.c reads it at the start)
    // (rows renamed since their values were first saved keep their keys: an options file from before still loads)
    if (r.value == &opts.resolution) return "resolution";
    if (r.value == &opts.fps_cap) return "frame_rate_cap";   // (host/nn_vi.c reads it at the start)
    if (r.value == &opts.upscale) return "upscaling";
    if (r.value == &opts.dlss) return "dlss";   // (as opts.dlss and RX_VK_DLSS)
    // ((Triforce) NX2navi's key, not the name cut at 31 letters; vk_replay.cpp tail_wanted reads it at the start)
    if (!strcmp(r.name, "Start next frame early")) return "tail_early";
    static char k[32];
    size_t n = 0;
    for (const char* p = r.name; *p && n + 1 < sizeof k; p++) k[n++] = *p == ' ' ? '_' : (char)tolower(*p);
    k[n] = 0;
    return k;
}
/// (Triforce) A row kept in triforce_options.ini: every row with a value but the Game language, whose choice is
/// language.txt's
bool ini_row(const Row& r) { return r.value && r.value != &opts.language; }
// ((Triforce, handoff §9) The options file's other writers. ini_base: what the game had for each of the file's lines when it last
// read (menu_init) or wrote (save) the file; save merges each line it writes by its key with what the file has now
// (ini_merge.h) and writes the file whole (file_whole.h: a reader at a start, the host's or the renderer's, never meets half a
// file). RX_VK_OPTIONS_MERGE=0: the game's own values written over the file in place, as before)
ini_merge::Map ini_base;
std::mutex ini_mu;   // (save runs on the presenting thread and, for the mouse's sensitivity keys, on the host's: option 7)
bool options_merge_on() {
    static const bool on = !(getenv("RX_VK_OPTIONS_MERGE") && !strcmp(getenv("RX_VK_OPTIONS_MERGE"), "0"));
    return on;
}
// (Triforce) the Game language row's file, beside the options file: the game folder's language.txt
std::string lang_path() {
    const std::string ini = ini_path();
    return ini.substr(0, ini.find_last_of("\\/") + 1) + "language.txt";
}
/// Its bytes as they are (64 KiB at most: a larger file is not that one)
bool lang_read(std::string& text) {
    constexpr size_t kMost = 65536;
    text.clear();
    FILE* f = fopen(lang_path().c_str(), "rb");
    if (!f) return false;
    char part[4096];
    for (size_t n; text.size() <= kMost && (n = fread(part, 1, sizeof part, f)) > 0;) text.append(part, n);
    fclose(f);
    return text.size() <= kMost;
}
/// The file written whole beside it and put in its place, so that a write that fails leaves the file as it was
bool lang_write(const std::string& text) {
    const std::string path = lang_path(), fresh = path + ".new";
    FILE* f = fopen(fresh.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = fclose(f) == 0 && ok;
#ifdef _WIN32
    ok = ok && MoveFileExA(fresh.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    ok = ok && rename(fresh.c_str(), path.c_str()) == 0;
#endif
    if (!ok) remove(fresh.c_str());
    return ok;
}
/// The row from the file, at the start: its entries, and the one the file chooses (English (US) when it chooses none, as
/// the host answers then)
void lang_load() {
    std::string text;
    lang_count = 0, lang_saved = -1;
    if (lang_read(text)) {
        const gl::File f = gl::read(text);
        lang_count = gl::offered(f, lang_entries);
        const int now = f.chosen >= 0 ? f.chosen : gl::kDefault;
        for (int i = 0; i < lang_count; i++)
            if (lang_entries[i] == now) lang_saved = i;
    }
    if (lang_saved < 0) lang_count = 0;
    opts.language = std::max(lang_saved, 0);
    for (Row& r : rows)
        if (r.value == &opts.language) r.hi = std::max(lang_count - 1, 0);
}
/// At a save: the row's choice into language.txt, when it is not the file's already. The file is read again first (it may
/// have been edited since the start) and only its choice line changes; a language it no longer names is not written, and
/// the row goes back to the file's
void lang_store() {
    if (lang_count < 2 || opts.language == lang_saved || opts.language < 0 || opts.language >= lang_count) return;
    const int language = lang_entries[opts.language];
    std::string text;
    bool ok = false;
    if (lang_read(text)) {
        const std::string out = gl::write(text, language);
        ok = gl::read(out).chosen == language && (out == text || lang_write(out));
    }
    lang_failed = !ok;
    if (ok) {
        lang_saved = opts.language;
        logf("menu: the game's language from the next start: %s (language.txt)", gl::kLanguages[language].code);
    } else {
        logf("menu: language.txt could not be written, or no longer names %s: the game's language stays as it was",
             gl::kLanguages[language].code);
        opts.language = lang_saved;
    }
}
// (hookshot) the controls, kept by the host (host/nn_hid.c reads them back from here): pad_<button>, key_<button>
const char* const bind_names[HOS_KEY_N] = {"a", "b", "x", "y", "ls", "rs", "l", "r", "zl", "zr", "plus", "minus",
                                           "left", "up", "right", "down", "move_up", "move_down", "move_left",
                                           "move_right", "cam_up", "cam_down", "cam_left", "cam_right"};
bool can_bind() { return host && host->binding && host->bind && host->pad_raw; }
const char* tx_pad_layout(int v) {
    static const char* t[] = {"Nintendo (Def)", "Xbox / PC", "Swap Face", "Custom"};
    return t[std::min(std::max(v, 0), 3)];
}
void apply_pad_layout(int layout) {
    if (!can_bind()) return;
    if (layout == 0) { // Nintendo (Default: Bottom=B, Right=A, Top=X, Left=Y)
        host->bind(HOS_BIND_PAD, HOS_SW_A, 0, HOS_PAD_EAST);
        host->bind(HOS_BIND_PAD, HOS_SW_B, 0, HOS_PAD_SOUTH);
        host->bind(HOS_BIND_PAD, HOS_SW_X, 0, HOS_PAD_NORTH);
        host->bind(HOS_BIND_PAD, HOS_SW_Y, 0, HOS_PAD_WEST);
    } else if (layout == 1) { // Xbox / PC Standard (Bottom=A, Right=B, Left=X, Top=Y)
        host->bind(HOS_BIND_PAD, HOS_SW_A, 0, HOS_PAD_SOUTH);
        host->bind(HOS_BIND_PAD, HOS_SW_B, 0, HOS_PAD_EAST);
        host->bind(HOS_BIND_PAD, HOS_SW_X, 0, HOS_PAD_WEST);
        host->bind(HOS_BIND_PAD, HOS_SW_Y, 0, HOS_PAD_NORTH);
    } else if (layout == 2) { // Swap Face Buttons (Bottom=A, Right=B, Top=X, Left=Y)
        host->bind(HOS_BIND_PAD, HOS_SW_A, 0, HOS_PAD_SOUTH);
        host->bind(HOS_BIND_PAD, HOS_SW_B, 0, HOS_PAD_EAST);
        host->bind(HOS_BIND_PAD, HOS_SW_X, 0, HOS_PAD_NORTH);
        host->bind(HOS_BIND_PAD, HOS_SW_Y, 0, HOS_PAD_WEST);
    }
}
int detect_pad_layout() {
    if (!can_bind()) return opts.pad_layout;
    const int a = host->binding(HOS_BIND_PAD, HOS_SW_A, 0);
    const int b = host->binding(HOS_BIND_PAD, HOS_SW_B, 0);
    const int x = host->binding(HOS_BIND_PAD, HOS_SW_X, 0);
    const int y = host->binding(HOS_BIND_PAD, HOS_SW_Y, 0);
    if (a == HOS_PAD_EAST && b == HOS_PAD_SOUTH && x == HOS_PAD_NORTH && y == HOS_PAD_WEST) return 0;
    if (a == HOS_PAD_SOUTH && b == HOS_PAD_EAST && x == HOS_PAD_WEST && y == HOS_PAD_NORTH) return 1;
    if (a == HOS_PAD_SOUTH && b == HOS_PAD_EAST && x == HOS_PAD_NORTH && y == HOS_PAD_WEST) return 2;
    return 3;
}
// (hookshot) the row's choice to the host: off, the default, or a microphone by its name (and its place in the host's list,
// -1 while it is unplugged: of two with one name, the one chosen). Unplugged, the name is the kept one, cut: the host takes
// a name that starts with it
void mic_tell_host() {
    if (!can_mic()) return;
    if (opts.mic <= 1) host->mic_select(opts.mic <= 0 ? 0 : 1, "", -1);
    else if (mic_missing && opts.mic == 2 + mic_listed) host->mic_select(2, mic_want.c_str(), -1);   // (the kept name, cut)
    else host->mic_select(2, host->mic_name(opts.mic - 2), opts.mic - 2);   // (its whole name: the ini's cut is only the ini's)
}
void mic_row_ends() {
    for (Row& r : rows)
        if (r.value == &opts.mic) r.hi = 1 + mic_listed + (mic_missing ? 1 : 0);
}
// (hookshot) the microphones listed again (one plugged in since shows up), the row on mic_want's: the one at the row's place
// when it still has that name (two microphones of one name: the second stays chosen), else the first of that name, else
// its "(not connected)" entry while it is unplugged
void refresh_mics() {
    if (!can_mic()) {
        if (opts.mic >= 2) opts.mic = 1;
        return;
    }
    mic_listed = std::min(host->mic_count(), kMaxMics);
    int at = -1;
    if (!mic_want.empty()) {
        if (opts.mic >= 2 && opts.mic - 2 < mic_listed && mic_key(host->mic_name(opts.mic - 2)) == mic_want) at = opts.mic - 2;
        for (int i = 0; i < mic_listed && at < 0; i++)
            if (mic_key(host->mic_name(i)) == mic_want) at = i;
        opts.mic = at >= 0 ? at + 2 : 2 + mic_listed;
    } else if (opts.mic >= 2) {
        opts.mic = 1;
    }
    mic_missing = !mic_want.empty() && at < 0;
    mic_row_ends();
    mic_tell_host();
}
// (the row changed by the player: the choice is now this one, by its name; the "(not connected)" entry is kept only while
// it is the one chosen)
void mic_chosen() {
    if (opts.mic >= 2 && !(mic_missing && opts.mic == 2 + mic_listed)) {
        mic_want = mic_key(host->mic_name(opts.mic - 2)), mic_missing = false;
    } else if (opts.mic < 2) {
        mic_want.clear(), mic_missing = false;
    }
    mic_row_ends();
    mic_tell_host();
}
void save() {
    lang_store();   // (Triforce: the Game language row's choice is language.txt's, not this file's)
    const std::lock_guard ini_lock(ini_mu);
    const bool merge = options_merge_on();
    // (hookshot) the lines that are not the menu's are kept: the controls, pad_ and key_ (host/nn_hid.c)
    // (and every other line the menu does not write, such as discord_app_id=: they were dropped at the next save)
    std::string kept, others;
    ini_merge::Map disk;   // ((Triforce) the file's lines as they are now)
    if (FILE* f = fopen(ini_path().c_str(), "r")) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            ini_merge::add_line(disk, line);
            if (!strncmp(line, "pad_", 4) || !strncmp(line, "key_", 4)) {
                kept += line;
                continue;
            }
            const char* eq = strchr(line, '=');
            if (!eq || eq == line) continue;
            const std::string key(line, (size_t)(eq - line));
            bool mine = key == "microphone_device" || key == "options_version" || key == "debug_tab";
            // (Triforce: the RivaTuner limit row of the test builds is gone, RivaTuner being the limiter wherever it can be:
            // its line is read by nothing and is not written again)
            mine |= key == "rivatuner_limit";
            for (const Row& r : rows)
                if (!mine && r.value && key == ini_key(r)) mine = true;
            if (!mine) others += line, others += others.back() == '\n' ? "" : "\n";
        }
        fclose(f);
    }
    // ((Triforce) each line the game writes: its value, or the file's where it was changed outside and not in the game since
    // (ini_merge::pick); now: the game's own values, the base for the next save)
    std::string out, outside;
    ini_merge::Map now;
    const auto put = [&](const std::string& key, const std::string& value) {
        bool taken = false;
        out += key + "=" + (merge ? ini_merge::pick(key, value, ini_base, disk, &taken) : value) + "\n";
        now[key] = value;
        if (taken) outside += (outside.empty() ? "" : ", ") + key;
    };
    for (const Row& r : rows)
        if (ini_row(r)) put(ini_key(r), std::to_string(*r.value));
    if (!mic_want.empty()) put("microphone_device", mic_want);   // (hookshot: also while it is unplugged)
    // (2: game_speed below 2 was only ever the old default, see menu_init; 3: the mouse's buttons are bindings, see
    // host/nn_hid.c binds_load; 4: Descriptor reuse on by default, see menu_init and vk_draw.cpp desc_slotgen_on; 5: Sampler
    // cache, Fast pipeline link, Lazy write-back, Opaque textures as BC1 and NVIDIA Reflex On + Boost by default, see menu_init;
    // (Triforce) 6: Start next frame early on by default, see menu_init and vk_replay.cpp tail_wanted; the Ultrawide row's 1 is
    // 16:10, no longer the monitor's shape, see ultrawide_rows.h; (Triforce) 7: Frame pacing on by default, see menu_init)
    out += "options_version=7\n";
    out += others;
    if (!can_bind()) out += kept;
    for (int i = 0; i < HOS_KEY_N && can_bind(); i++) {   // (the keys-only controls, HOS_KEY_*, have no pad_ line)
        if (i < HOS_SW_N) put(std::string("pad_") + bind_names[i], std::to_string(host->binding(HOS_BIND_PAD, i, 0)));
        put(std::string("key_") + bind_names[i],
            std::to_string(host->binding(HOS_BIND_KEY, i, 0)) + "," + std::to_string(host->binding(HOS_BIND_KEY, i, 1)));
    }
#ifdef _WIN32
    {   // (the line ends the file always had there: it was written in text mode)
        std::string crlf;
        for (const char ch : out) crlf += ch == '\n' ? std::string("\r\n") : std::string(1, ch);
        out.swap(crlf);
    }
#endif
    const auto in_place = [&out] {   // (as before: over the file itself)
        FILE* f = fopen(ini_path().c_str(), "wb");
        if (!f) return false;
        const bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
        return fclose(f) == 0 && ok;
    };
    // ((Triforce) whole, beside it and then in its place; in place where that cannot be (another program holds the file open
    // without letting it be replaced), so that the change is not lost)
    bool written = merge && file_writers::write_whole(ini_path(), out);
    if (!written && !file_writers::stopping()) written = in_place();
    if (written) ini_base = std::move(now);
    static std::string said;   // (once for each set of such lines, not at every save)
    if (!outside.empty() && outside != said)
        logf("menu: options: %s changed in triforce_options.ini outside the game since it read the file, and not in the game: kept as "
             "the file has them (the game takes them at its next start; RX_VK_OPTIONS_MERGE=0: the game's own)", outside.c_str());
    said = outside;
}

/// A row's new value v, as the arrows set it (dir: 1 up, -1 down, for the rows that skip or wrap), saved unless !keep
/// (a slider's drag: saved once it is let go)
void set_row(Row& r, int v, int dir, bool keep = true) {
    // (nothing else to choose: no change, no save. A saved choice that cannot run here is kept)
    if (row_fixed(r.value)) return;
    // (the scaling rows: only what can run here; the Render resolution row through the Upscaler's own steps)
    if (r.value == &opts.dlss) v = ur::step_upscaler(upscaler(), dir, upscalers_available());
    if (r.value == &opts.resolution) v = ur::step(upscaler(), *r.value, dir);
    if (r.value == &opts.fg_engine) v = fgengine::step_row(*r.value, dir, fg_engines_available());   // (what runs here)
    if (r.value == &opts.output_res) {   // (smallest to largest: kOutputOrder)
        v = step_output_choice(*r.value, dir);
        if (v == *r.value) return;
    }
    if (stops_at_ends(r)) v = std::min(std::max(v, r.lo), r.hi);   // (a scale: it stops at its ends)
    if (r.value == &opts.pad_layout) {
        if (v > 2) v = dir > 0 ? 0 : 2;
        if (v < 0) v = 2;
        *r.value = v;
        apply_pad_layout(*r.value);
    } else if (r.value == &opts.preset) {
        *r.value = v > r.hi ? r.lo : v < r.lo ? r.hi : v;
        apply_preset(*r.value);
    } else {
        *r.value = v > r.hi ? r.lo : v < r.lo ? r.hi : v;
        // (another Upscaler: the Render resolution moves to the nearest of its steps, DLSS's modes or the game's)
        if (r.value == &opts.dlss) opts.resolution = ur::snap(upscaler(), opts.resolution);
        if (!r.preset_exempt) {   // (the controls, the window and the counter are not the preset's)
            opts.preset = 0; // Custom
            sync_preset_budgets(0);
        }
    }
    if (r.value == &opts.mic && can_mic()) mic_chosen();   // (hookshot)
    {   // (a row that takes a restart, or a preset that sets one: asked again at the next close)
        int now[mr::kMax];
        restart_values(now);
        restart_state.note(now);
    }
    if (keep) save();
}

// ---- (hookshot) the Controls page: what presses each of the Switch's buttons, a gamepad input and up to two keys ----
struct Control {
    int button;   // HOS_SW_*, or HOS_KEY_* (keys only)
    const char* name;
};
const Control controls[] = {{HOS_SW_A, "A"}, {HOS_SW_B, "B"}, {HOS_SW_X, "X"}, {HOS_SW_Y, "Y"}, {HOS_SW_L, "L"},
                            {HOS_SW_R, "R"}, {HOS_SW_ZL, "ZL"}, {HOS_SW_ZR, "ZR"}, {HOS_SW_PLUS, "Plus"},
                            {HOS_SW_MINUS, "Minus"}, {HOS_SW_LS, "Left stick press"}, {HOS_SW_RS, "Right stick press"},
                            {HOS_SW_UP, "D-pad up"}, {HOS_SW_DOWN, "D-pad down"}, {HOS_SW_LEFT, "D-pad left"},
                            {HOS_SW_RIGHT, "D-pad right"}, {HOS_KEY_MOVE_UP, "Move forward"},
                            {HOS_KEY_MOVE_DOWN, "Move back"}, {HOS_KEY_MOVE_LEFT, "Move left"},
                            {HOS_KEY_MOVE_RIGHT, "Move right"}, {HOS_KEY_CAM_UP, "Camera up"},
                            {HOS_KEY_CAM_DOWN, "Camera down"}, {HOS_KEY_CAM_LEFT, "Camera left"},
                            {HOS_KEY_CAM_RIGHT, "Camera right"}};
constexpr int kControls = sizeof controls / sizeof controls[0];   // (then "Reset to defaults")
bool keys_only(int i) { return i < kControls && controls[i].button >= HOS_SW_N; }   // (no gamepad input for those)
// a pad input's name on the controller of the button prompts (the Button prompts row's choice; Auto: the face in use),
// else by its place ((Triforce) by its place too with the keyboard and mouse face, which is no controller's)
const char* pad_name(int p) {
    static const char* t[HOS_PROMPT_KEYBOARD + 1][HOS_PAD_N] = {   // (by place, then HOS_PROMPT_*, the controllers')
        {"Bottom button", "Right button", "Left button", "Top button", "Select / Back", "Home", "Start", "Left stick press",
         "Right stick press", "L1 / LB", "R1 / RB", "D-pad up", "D-pad down", "D-pad left", "D-pad right", "L2 / LT", "R2 / RT"},
        {"B", "A", "Y", "X", "Minus", "Home", "Plus", "Left stick press", "Right stick press", "L", "R", "D-pad up",
         "D-pad down", "D-pad left", "D-pad right", "ZL", "ZR"},
        {"A", "B", "X", "Y", "View", "Xbox", "Menu", "Left stick press", "Right stick press", "LB", "RB", "D-pad up",
         "D-pad down", "D-pad left", "D-pad right", "LT", "RT"},
        {"Cross", "Circle", "Square", "Triangle", "Share", "PS", "Options", "L3", "R3", "L1", "R1", "D-pad up", "D-pad down",
         "D-pad left", "D-pad right", "L2", "R2"},
        {"Cross", "Circle", "Square", "Triangle", "Create", "PS", "Options", "L3", "R3", "L1", "R1", "D-pad up", "D-pad down",
         "D-pad left", "D-pad right", "L2", "R2"},
        {"A", "B", "X", "Y", "View", "Steam", "Menu", "L3", "R3", "L1", "R1", "D-pad up", "D-pad down", "D-pad left",
         "D-pad right", "L2", "R2"}};
    const int face = opts.prompts ? opts.prompts - 1 : host && host->prompt_face ? host->prompt_face() : -1;
    return p >= 0 && p < HOS_PAD_N ? t[face >= 0 && face < HOS_PROMPT_KEYBOARD ? face + 1 : 0][p] : "None";
}
const char* key_name(int vk) {
    static char b[16];
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return b[0] = (char)vk, b[1] = 0, b;
    if (vk >= VK_F1 && vk <= VK_F12) return snprintf(b, sizeof b, "F%d", vk - VK_F1 + 1), b;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD0 + 9) return snprintf(b, sizeof b, "Num %d", vk - VK_NUMPAD0), b;
    switch (vk) {
    case VK_SPACE: return "Space";
    case VK_SHIFT: return "Shift";
    case VK_CONTROL: return "Ctrl";
    case VK_MENU: return "Alt";
    case VK_TAB: return "Tab";
    case VK_RETURN: return "Enter";
    case VK_BACK: return "Backspace";
    case VK_CAPITAL: return "Caps Lock";
    case VK_UP: return "Up";
    case VK_DOWN: return "Down";
    case VK_LEFT: return "Left";
    case VK_RIGHT: return "Right";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "Page Up";
    case VK_NEXT: return "Page Down";
    case VK_ADD: return "Num +";
    case VK_SUBTRACT: return "Num -";
    case VK_MULTIPLY: return "Num *";
    case VK_DIVIDE: return "Num /";
    case VK_DECIMAL: return "Num .";
    case VK_OEM_1: return ";";
    case VK_OEM_PLUS: return "=";
    case VK_OEM_COMMA: return ",";
    case VK_OEM_MINUS: return "-";
    case VK_OEM_PERIOD: return ".";
    case VK_OEM_2: return "/";
    case VK_OEM_3: return "`";
    case VK_OEM_4: return "[";
    case VK_OEM_5: return "\\";
    case VK_OEM_6: return "]";
    case VK_OEM_7: return "'";
    case VK_LBUTTON: return "Left click";
    case VK_RBUTTON: return "Right click";
    case VK_MBUTTON: return "Middle click";
    case 0x05: return "Mouse 4";   // (VK_XBUTTON1)
    case 0x06: return "Mouse 5";
    }
    return snprintf(b, sizeof b, "Key %d", vk), b;
}
// the keys a binding can take (Esc cancels, Insert and F1 are the menu's, Delete clears)
const std::vector<int>& bindable_keys() {
    static std::vector<int> k;
    if (k.empty()) {
        for (int c = 'A'; c <= 'Z'; c++) k.push_back(c);
        for (int c = '0'; c <= '9'; c++) k.push_back(c);
        for (int c = VK_F2; c <= VK_F12; c++) k.push_back(c);
        for (int c = VK_NUMPAD0; c <= VK_NUMPAD0 + 9; c++) k.push_back(c);
        for (int c : {VK_SPACE, VK_SHIFT, VK_CONTROL, VK_MENU, VK_TAB, VK_RETURN, VK_BACK, VK_CAPITAL, VK_UP, VK_DOWN,
                      VK_LEFT, VK_RIGHT, VK_HOME, VK_END, VK_PRIOR, VK_NEXT, VK_ADD, VK_SUBTRACT, VK_MULTIPLY,
                      VK_DIVIDE, VK_DECIMAL, VK_OEM_1, VK_OEM_PLUS, VK_OEM_COMMA, VK_OEM_MINUS, VK_OEM_PERIOD,
                      VK_OEM_2, VK_OEM_3, VK_OEM_4, VK_OEM_5, VK_OEM_6, VK_OEM_7, VK_LBUTTON, VK_RBUTTON, VK_MBUTTON,
                      0x05, 0x06})   // (the mouse's buttons too: they count while the mouse is captured)
            k.push_back(c);
    }
    return k;
}
int page = 0;          // 0 the options, 1 the controls, 3 the teleport list, 4 the inventory, 5 the restart question (2: was the
                       // debug tools' own page, now on the Debug tab), 6 (Triforce) a name page: Game flags, Cutscenes, Scan amiibo
int csel = 0, ccol = 0;   // the page's row (kTop first: its own rows, then the controls, then reset) and column (0 the
                          // gamepad, 1 the keyboard)
const int kTop = tab_row_count(TAB_PAGE);   // (Layout preset and Button prompts: a preset shows in the grid at once)
int listening = 0;     // waiting for the new input: 1 a gamepad's, 2 a key
bool listen_armed = false;   // (once what started it was let go)
int64_t listen_end = 0;
std::string keys_text(int button) {
    const int a = host->binding(HOS_BIND_KEY, button, 0), b = host->binding(HOS_BIND_KEY, button, 1);
    std::string t = a ? key_name(a) : "";
    if (b) t += std::string(a ? ", " : "") + key_name(b);
    return t.empty() ? "None" : t;
}

// ---- what is drawn: Dear ImGui over the window's image (vk_imgui.h) in the game's own style (vk_ui.h). Built anew at each
// present while something shows, from the menu's state (menu_tick and menu_record run on the presenting thread, one at a time:
// present_image's lock); nothing at all while nothing shows ----
std::mutex startup_mu;   // (the start-up screen's view, set from its own thread)
StartupView startup;
std::atomic<bool> startup_on{false};
std::atomic<float> startup_fade{0};   // (the start-up screen's fade to black at its end, 0 to 1: startup_fade_to)
std::atomic<int64_t> game_frames{0}, shown_frames{0};
char fps_text[48] = "";
char fps_sub[32] = "";
// (Triforce) The performance overlay, the FPS counter row on Full (opts.show_fps == 2; draw_perf): NX2navi's full panel. The
// game's frame times go into the ring as the frames are counted (menu_note_frame), under perf_mu: with frame generation that
// is the display thread, else the replay's. The machine's numbers are sampled on a thread of their own while the row is on
// Full (perf_machine.cpp, started and stopped in menu_tick). The panel's texts are made twice a second where the counter's
// number is (menu_tick, perf_sample) and drawn by the same thread. Nothing of it runs while the row is not on Full: one
// comparison a frame
std::mutex perf_mu;
perf_stats::Ring perf_ring;
constexpr int kPerfGraph = 120;   // (the graph's frames: NX2navi's PERF_HISTORY)
// (the game's version: Triforce makes the game from build e0f3ccfa alone, the game's 1.1.1 (nx2/make_game.py BUILD_ID,
// check_supported_build); the renderer is not told the build at run time)
constexpr const char* kGameVersion = "1.1.1";
// (the panel's rows, top to bottom, with NX2navi's names and in its order: the machine and the frames in three columns, then
// the figures per frame, then the settings that shape the picture (NX2navi's "3D scene" is hidden on this game: not here))
constexpr const char* kPerfTop[] = {"GPU", "VRAM", "CPU", "RAM", "FPS", "FG", "Frame"};
constexpr int kPerfTopRows = 7, kPerfFG = 5;
constexpr const char* kPerfFrame[] = {"GPU frame prep", "GPU busy per frame", "Pipeline waits", "Shader waits", "Texture uploads"};
constexpr int kPerfFrameRows = 5;
constexpr const char* kPerfSetting[] = {"Window", "Game image", "Upscaling", "Anti-aliasing", "Frame limit", "VSync",
                                        "Widescreen", "Resolution", "HDR output", "Frame generation"};
constexpr int kPerfSettings = 10;
// (the panel's opacity: the options' caption plate under it at the share the options dim the game by (ui::kDimAlpha), so the
// game shows through it as through the dim; its texts and dividers are drawn whole)
constexpr float kPerfOpacity = ui::kDimAlpha / 255.0f;
/// (Triforce) The panel's scale, NX2navi's menu_scale: the window's lines over 1080 (on a window narrower than 1.47:1, 0.68 of
/// its width's), and at least 720 lines at the window's display scale (its DPI over 96: sizes are physical pixels, so a small
/// window on a 150% display still has text as large as the display asks for). draw_perf and prebake both size by it
float perf_scale(float w, float h) {
    float dpi = 96;
#ifdef _WIN32
    typedef UINT(WINAPI * GetDpi)(HWND);   // (Windows 10 1607 on: looked up, as host/display.c window_dpi does)
    static const GetDpi get = (GetDpi)(void*)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow");
    if (const UINT d = get ? get(app_window()) : 0) dpi = (float)d;
#endif
    return std::max(std::min(h, w * 0.68f), 720.0f * dpi / 96.0f) / 1080.0f;
}
/// The panel's text size at scale k: its menu font's 25.5 at the window's 0.72 (NX2navi's SetWindowFontScale)
float perf_px(float k) { return 25.5f * 0.72f * k; }
struct PerfCell {
    char t[32] = "";   // (the text; empty: an empty cell)
    bool dim = false;  // ("-": the machine or the game gives no number)
};
struct PerfView {
    bool have = false;                         // (made: false until the first sample)
    bool full = false;                         // (the row was on Full at the sample before: a ring from an earlier time is emptied)
    char title[96] = "";                       // ("Triforce <this build>, game 1.1.1": perf_title)
    PerfCell top[kPerfTopRows][3];             // (GPU, VRAM, CPU, RAM; FPS, FG, Frame: three cells each)
    bool fg = false;                           // (the FG row shows: frame generation makes frames)
    PerfCell frame[kPerfFrameRows];            // (one each)
    char setting[kPerfSettings][48] = {};      // (kPerfSetting's values; an empty one: its row is left out, as NX2navi's)
    char gpu_name[96] = "", cpu_name[64] = "";
    // (the running counts as of the sample before: the figures per frame are what came since, over the game's frames since,
    // perf_stats::per_frame)
    perf_stats::Base base;
} perf_view;
menu_hit::Map hits;      // (the menu's controls as last drawn: the mouse's, menu_tick)
float scroll_at[TAB_MOTION + 2];   // (the first row shown, eased toward the selection: the options' tabs and the Motion page, then
                                   // the Controls page: kControlsSlot)
constexpr int kControlsSlot = TAB_MOTION + 1;
int wheel_rows[TAB_MOTION + 2];   // (the mouse wheel's notches since the last drawing, a slot each: scroll_rows scrolls by them)
int scrolled_to[TAB_MOTION + 2] = {-1, -1, -1, -1, -1, -1, -1, -1};   // (the selection scroll_rows last brought into view)
static_assert(TAB_MOTION + 2 == 8, "scrolled_to: a slot for each tab, the Motion page and the Controls page");
float scroll_want[TAB_MOTION + 2];   // (where scroll_at is going: the wheel moves it, a new selection too)
float ease_step = 0;     // (this drawing's share of the way: the time since the last one)
bool ease_live = false;  // ((Triforce) menu_record steps the easing: a window's frames. Without them a still is drawn settled)

ui::Ctx ui_ctx(uint32_t w, uint32_t h) {
    ui::Ctx c{ImGui::GetForegroundDrawList(), ui::fonts(), 1, (float)w, (float)h, 0, pad_last};
    c.confirm_dot = menu_keys::dot(pad_confirm), c.back_dot = menu_keys::dot(pad_back);
    c.s = ui::unit((float)h, 0, false, false, &c.safe);
    // (the cursor's glow breathes, as the game's: a 2 s cycle; RX_VK_UI_STILL=1 holds it, for pictures that compare)
    static const bool still = getenv("RX_VK_UI_STILL") != nullptr;
    c.pulse = still ? 0.6f : 0.5f + 0.5f * std::sin((float)((prof_now() % 2000000000LL) / 2e9 * 6.2831853));
    return c;
}
/// (Triforce) The menu's, the counter's and the notice's: the game's menu fonts where the pages draw their text (body its
/// "Normal", strong its "Special"); the start-up screen keeps ui_ctx's
ui::Ctx menu_ctx(uint32_t w, uint32_t h) {
    ui::Ctx c = ui_ctx(w, h);
    if (c.f.menu) c.f.body = c.f.menu, c.f.strong = c.f.heading;
    return c;
}
/// The header's height (SystemOverlay_00's P_SystemHeader_00: 200 units)
float banner_h(const ui::Ctx& c) { return ui::banner_height(c); }
// (the caption's texts: PaGuideKey's 48 units for the help, a little smaller for the line about the value)
// (the game's caption is 54 units, 65 as drawn; ours say more than its one line, so a little smaller: 57 and 50)
constexpr float kHelpPt = 19.0f, kHintPt = 16.6f;

/// The tallest help a row of the options can have, in pixels at width: its help and the tallest line any of its values can
/// have: the help box keeps that height whichever row is selected and whatever its value
float measure_help(ui::Ctx& c, float width) {
    float most = 0;
    for (const Row& r : rows) {
        float hint = 0;
        for (int v = r.lo; r.value && r.hint && v <= r.hi; v++)
            for (int k = 0; const char* t = r.hint(v, k); k++)
                hint = std::max(hint, ui::para(c, c.f.body, kHintPt * c.s, ImVec2(), 0, t, width - 18 * c.s, false));
        if (r.help) most = std::max(most, ui::para(c, c.f.body, kHelpPt * c.s, ImVec2(), 0, r.help, width, false) + (hint ? 14 * c.s + hint : 0));
    }
    return most;
}
/// label in room pixels: as it is when it fits; the selected row's slides through it (marquee_pos, marquee_max for menu_tick);
/// another's ends with "..."
/// (UTF-8: the byte offsets where its characters start, and the end; labels are cut and slid a character at a time, never
/// inside one: a microphone's name can be in any script)
std::vector<size_t> char_starts(const std::string& t) {
    std::vector<size_t> at;
    for (size_t i = 0; i < t.size(); i++)
        if (((unsigned char)t[i] & 0xC0) != 0x80) at.push_back(i);
    at.push_back(t.size());
    return at;
}
std::string fit(ui::Ctx& c, ImFont* f, float px, float room, const std::string& label, bool sel) {
    auto width = [&](const std::string& t) { return ui::size_of(f, px, t.c_str()).x; };
    if (width(label) <= room) return label;
    const std::vector<size_t> at = char_starts(label);
    const int chars = (int)at.size() - 1;
    if (sel) {   // (marquee_pos and marquee_max count characters)
        int start = chars;   // (the last start that still shows the label's end)
        while (start > 0 && width(label.substr(at[start - 1])) <= room) start--;
        marquee_max = start;
        const int from = std::min(marquee_pos, marquee_max);
        int end = chars;
        while (end > from + 1 && width(label.substr(at[from], at[end] - at[from])) > room) end--;
        return label.substr(at[from], at[end] - at[from]);
    }
    int end = chars;
    while (end > 1 && width(label.substr(0, at[end]) + "...") > room) end--;
    std::string t = label.substr(0, at[end]);
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t + "...";
}
/// A list of rows from top to bottom (pixels), n of them pitch apart, the selection sel kept in view: the first one shown,
/// eased toward its place (scroll_at[slot]); *shown how many fit
float scroll_rows(int slot, int n, int sel, float top, float bottom, float pitch, int* shown) {
    const int fit = std::max(1, (int)((bottom - top) / pitch));
    *shown = fit;
    float& at = scroll_at[slot];
    float& want = scroll_want[slot];
    const int wheel = std::exchange(wheel_rows[slot], 0);
    if (n <= fit) want = 0;   // (nothing to scroll: the wheel does nothing)
    else {
        want -= (float)wheel;   // (the wheel scrolls the rows and leaves the selection, and every value, as they are)
        static int fits[sizeof scroll_at / sizeof *scroll_at];   // (and after a resize: the rows that fit changed)
        if (sel != scrolled_to[slot] || fit != std::exchange(fits[slot], fit)) {   // (a new selection, by the keys or the pad: brought into view)
            const int margin = fit > 4 ? 1 : 0;   // (a row of context above and below the selection)
            if (sel - margin < want) want = (float)(sel - margin);
            if (sel + margin + 1 > want + fit) want = (float)(sel + margin + 1 - fit);
        }
        want = std::round(std::clamp(want, 0.0f, (float)(n - fit)));
    }
    scrolled_to[slot] = sel;
    at += (want - at) * ease_step;
    if (std::fabs(want - at) < 0.01f) at = want;
    return at;
}
/// The scroll bar at x, beside a list of n rows of which shown fit, the first at `first`: the game's (PaScrollBar_00: a short
/// bar, 32 x 312 units, at the list's middle; its lit part the rows in view)
void scroll_bar(ui::Ctx& c, float x, float top, float bottom, int n, int shown, float first) {
    if (n <= shown && !c.over_game) return;   // (over the game's own list its bar is under ours: drawn, whole, even so)
    if (n <= shown) n = shown = 1, first = 0;
    const float mid = (top + bottom) * 0.5f, half = ui::lu(c, 156), w = ui::lu(c, 16);
    const float y0 = mid - half + 2 * half * first / n, y1 = mid - half + 2 * half * (first + shown) / n;
    if (const ui::Art* t = ui::art("ScrollBarBase_00")) {
        ui::image(c, t, ImVec2(x - w, mid - half), ImVec2(x + w, mid + half), ui::rgba(18, 14, 9, 235));
        c.d->PushClipRect(ImVec2(x - w, y0), ImVec2(x + w, y1), true);
        ui::image(c, t, ImVec2(x - w, mid - half), ImVec2(x + w, mid + half), ui::rgba(200, 146, 66));
        c.d->PopClipRect();
        return;
    }
    c.d->AddRectFilled(ImVec2(x - w * 0.4f, mid - half), ImVec2(x + w * 0.4f, mid + half), ui::rgba(18, 14, 9, 235), w * 0.4f);
    c.d->AddRectFilled(ImVec2(x - w * 0.4f, y0), ImVec2(x + w * 0.4f, y1), ui::rgba(200, 146, 66), w * 0.4f);
}
/// The header with a page's title in the middle (the Controls, Motion and list pages: as the game's "Load"), its height
float title_banner(ui::Ctx& c, const char* title, const char* right) {
    ui::banner(c, 0, title);
    if (right) ui::text_mid_right(c, c.f.body, ui::lu(c, 50), ui::at(c, 1800, 0).x, ui::at(c, 0, 990).y, ui::kBannerInk, right);
    return banner_h(c);
}
/// The caption plate from (x0, y0) to x1 (what shows of it): a title when the page has one, the help, a line about the value
/// set off by a bar, a line at its foot; the text in the plate's middle, as the game sets its caption. min_h: its text's
/// room at least; kept between y_min and y_max. bleed: how far the plate goes on to the right of x1, off the screen (the
/// game's caption plate does)
void help_box(ui::Ctx& c, float x0, float x1, float y0, const char* title, const char* help, const char* hint, const char* gold_line,
              float min_h = 0, bool after_restart = false, float y_min = 0, float y_max = FLT_MAX, float bleed = 0) {
    const float s = c.s, pad_x = ui::lu(c, 72), pad_y = ui::lu(c, 56), w = x1 - x0 - 2 * pad_x;
    const float th = help ? ui::para(c, c.f.body, kHelpPt * s, ImVec2(), 0, help, w, false) : 0,
                hh = hint ? ui::para(c, c.f.body, kHintPt * s, ImVec2(), 0, hint, w - 18 * s, false) : 0;
    const float text_h = th + (hint ? 14 * s + hh : 0), note = after_restart ? 30 * s : 0;
    const float content = (title ? 34 * s : 0) + note + text_h + (gold_line ? 46 * s : 0);
    float tall = std::max(min_h, content + 2 * pad_y);
    if (y0 + tall > y_max) y0 = std::max(y_min, y_max - tall);
    const float y1 = std::min(y0 + tall, y_max);
    ui::box(c, ImVec2(x0, y0), ImVec2(x1 + bleed, y1));
    c.d->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    float y = std::max(y0 + pad_y * 0.5f, (y0 + y1 - content) * 0.5f);
    if (title) ui::text(c, c.f.strong, 21.6f * s, ImVec2(x0 + pad_x, y), ui::kInk, title), y += 34 * s;
    if (after_restart) {   // (this row's change applies at the next start: said here, not in the row, where it would cut the name)
        ui::diamond(c, ImVec2(x0 + pad_x + 5 * s, y + 10 * s), 4 * s, ui::kGoldHi);
        ui::text_mid(c, c.f.strong, kHintPt * s, x0 + pad_x + 16 * s, y + 10 * s, ui::kGoldHi, "Takes effect after a restart");
        y += note;
    }
    if (help) ui::para(c, c.f.body, kHelpPt * s, ImVec2(x0 + pad_x, y), ui::kInk, help, w);
    y += th;
    if (hint) {
        const float hy = y + 14 * s;
        c.d->AddRectFilled(ImVec2(x0 + pad_x, hy + 2 * s), ImVec2(x0 + pad_x + 2.5f * s, hy + hh - 2 * s), ui::kAccent, s);
        ui::para(c, c.f.body, kHintPt * s, ImVec2(x0 + pad_x + 14 * s, hy), ui::kSoft, hint, w - 18 * s);
        y = hy + hh;
    }
    if (gold_line) {
        y += 14 * s;
        c.d->AddLine(ImVec2(x0 + pad_x, y), ImVec2(x1 - pad_x, y), ui::alpha(ui::kEdge, 0.8f), std::max(1.0f, s));
        ui::diamond(c, ImVec2(x0 + pad_x + 5 * s, y + 18 * s), 4 * s, ui::kGoldHi);
        ui::text_mid(c, c.f.strong, kHintPt * s, x0 + pad_x + 16 * s, y + 18 * s, ui::kGoldHi, gold_line);
    }
    c.d->PopClipRect();
}
/// The caption of a page at the options' place (SystemOption_00's N_Caption: its plate 90 units under the screen's middle, off the
/// screen's right edge), as tall as its text asks, between the header and the guides
void caption(ui::Ctx& c, float hx0, float hx1, const char* title, const char* help, const char* hint, const char* gold_line, float min_h = 0,
             bool after_restart = false);
/// The game's Options layout (SystemOption_00): the list's column 2240 units wide, its middle 730 left of the screen's (drawn by
/// draw_menu, under the header); the caption plate to its right, what shows of it from 440 to the screen's edge
constexpr float kListX = -730;   // (N_List_00)
void columns(const ui::Ctx& c, float* x0, float* x1, float* hx0, float* hx1, bool* narrow) {
    *x0 = ui::at(c, kListX - 1120, 0).x, *x1 = ui::at(c, kListX + 1120, 0).x;
    *hx0 = ui::at(c, 456, 0).x, *hx1 = ui::at(c, 1850, 0).x;
    *narrow = false;
}
void column(ui::Ctx&, float, float, float) {}   // (the column is under the header: draw_menu draws it first)
void caption(ui::Ctx& c, float hx0, float hx1, const char* title, const char* help, const char* hint, const char* gold_line, float min_h,
             bool after_restart) {
    // (the game's: 476 units of plate about a line 90 under the screen's middle; taller, about the same line, for a long text)
    const float s = c.s, hh = banner_h(c), bar_top = ui::prompt_bar_top(c), w = hx1 - hx0 - 2 * ui::lu(c, 72);
    const float th = help ? ui::para(c, c.f.body, kHelpPt * s, ImVec2(), 0, help, w, false) : 0,
                nh = hint ? 14 * s + ui::para(c, c.f.body, kHintPt * s, ImVec2(), 0, hint, w - 18 * s, false) : 0;
    const float content = std::max(min_h, th + nh) + (title ? 34 * s : 0) + (after_restart ? 30 * s : 0) + (gold_line ? 46 * s : 0);
    const float tall = std::max(ui::lu(c, 476), content + 2 * ui::lu(c, 56)), mid = ui::at(c, 0, -90).y;
    const float y0 = std::max(hh + 14 * s, mid - tall * 0.5f);
    help_box(c, hx0, hx1, y0, title, help, hint, gold_line, tall, after_restart, hh + 14 * s, bar_top - 8 * s, ui::lu(c, 330));
}
/// A row's name on its plate; pending: a change that applies after a restart, the game's corner mark at its top left in
/// gold (the help says it in words). The selected row's plate is as the others': the game's cursor alone marks it
void name_cell(ui::Ctx& c, ImVec2 a, ImVec2 b, const char* name, bool sel, bool pending = false, float px = 18.0f * ui::kType) {
    const float s = c.s, mid = (a.y + b.y) * 0.5f;
    (void)sel;
    ui::plate(c, a, b);
    if (pending) {
        if (const ui::Art* t = ui::art("OptionCheckMark_00")) ui::image(c, t, a + ImVec2(ui::lu(c, -8), ui::lu(c, -8)), a + ImVec2(ui::lu(c, 82), ui::lu(c, 82)), ui::kGoldHi);
        else ui::diamond(c, ImVec2(a.x + 10 * s, a.y + 10 * s), 4 * s, ui::kGoldHi);
    }
    const float room = b.x - a.x - ui::lu(c, 110);
    const std::string shown = fit(c, c.f.body, px * s, room, name, false);
    ui::text_mid(c, c.f.body, px * s, a.x + ui::lu(c, 72), mid - ui::lu(c, 2), ui::kInk, shown.c_str());
}
/// A value, as the game's choices: the near-black panel a..b, on it the orange plate with the value in white and the game's
/// small arrows at its ends (left, right: whether it can go that way); the cursor round the panel when selected. fixed:
/// nothing else to choose here, dim. plain: no plate (a row that opens a page: its word and an arrow)
void choice_cell(ui::Ctx& c, ImVec2 a, ImVec2 b, const std::string& value, bool sel, bool left, bool right, bool fixed = false,
                 bool plain = false) {
    const float mid = (a.y + b.y) * 0.5f, px = std::min(ui::lu(c, 54 * ui::kType), (b.y - a.y) * 0.62f), in = ui::lu(c, 8);
    ui::value_plate(c, a, b);
    if (!plain) ui::value_plate(c, a + ImVec2(in, in), b - ImVec2(in, in), true, fixed ? 0.30f : 1.0f);
    const ImU32 ink = fixed ? ui::kDim : ui::kInk;
    if (left) ui::arrow(c, ImVec2(a.x + ui::lu(c, 40), mid), false, ink);
    if (right) ui::arrow(c, ImVec2(b.x - ui::lu(c, 40), mid), true, ink);
    const std::string shown = fit(c, c.f.body, px, b.x - a.x - ui::lu(c, left || right ? 150 : 50), value, sel);   // (room for arrows)
    ui::text_mid_centre(c, c.f.body, px, (a.x + b.x) * 0.5f, mid - ui::lu(c, 2), ink, shown.c_str());
    if (sel) ui::cursor(c, a - ImVec2(ui::lu(c, 2.5f), ui::lu(c, 3)), b + ImVec2(ui::lu(c, 2.5f), ui::lu(c, 3)));
}
/// (Triforce) Where a value's two halves meet (halves_cell): 606 of the panel's 98 to 902. The mouse's halves are split there too
float halves_meet(ImVec2 a, ImVec2 b) { return a.x + (b.x - a.x) * 0.632f; }
/// A value of two halves, as the game's toggles (On at the left, Off at the right): the one chosen on its orange plate in
/// white, the other dim on the panel; the left one's plate the wider (the layout's 516 and 320 of 836). high: the right
/// one is chosen
void halves_cell(ui::Ctx& c, ImVec2 a, ImVec2 b, const char* lo, const char* hi, bool high, bool sel, bool fixed) {
    const float mid = (a.y + b.y) * 0.5f, in = ui::lu(c, 8), px = std::min(ui::lu(c, 54 * ui::kType), (b.y - a.y) * 0.62f);
    const float xm = halves_meet(a, b);   // (where the two meet)
    ui::value_plate(c, a, b);
    const ImVec2 fa = high ? ImVec2(xm, a.y + in) : a + ImVec2(in, in), fb = high ? b - ImVec2(in, in) : ImVec2(xm, b.y - in);
    ui::value_plate(c, fa, fb, true, fixed ? 0.30f : 1.0f);
    const ImU32 on = fixed ? ui::kDim : ui::kInk;
    ui::text_mid_centre(c, c.f.body, px, (a.x + xm) * 0.5f, mid - ui::lu(c, 2), high ? ui::kDim : on, lo);
    ui::text_mid_centre(c, c.f.body, px, (xm + b.x) * 0.5f, mid - ui::lu(c, 2), high ? on : ui::kDim, hi);
    if (sel) ui::cursor(c, a - ImVec2(ui::lu(c, 2.5f), ui::lu(c, 3)), b + ImVec2(ui::lu(c, 2.5f), ui::lu(c, 3)));
}
/// A scale's value: as a choice with its arrows (the game's Options have no sliders: its levels are choices too), a fine
/// line under the value for how far along it is; *t0, *t1 the line (for the mouse)
void slider_cell(ui::Ctx& c, ImVec2 a, ImVec2 b, const char* value, int permille, bool sel, float* t0, float* t1) {
    choice_cell(c, a, b, value, sel, permille > 0, permille < 1000);
    const float x0 = a.x + ui::lu(c, 150), x1 = b.x - ui::lu(c, 150), y = b.y - ui::lu(c, 22), h = std::max(1.0f, ui::lu(c, 2.5f));
    c.d->AddRectFilled(ImVec2(x0, y - h), ImVec2(x1, y + h), ui::rgba(60, 36, 8, 110), h);
    c.d->AddRectFilled(ImVec2(x0, y - h), ImVec2(x0 + (x1 - x0) * permille / 1000.0f, y + h), ui::kInk, h);
    *t0 = x0, *t1 = x1;
}
/// "Restart needed: " and the pending rows, on one line of width: the last ones as "and N more" when they do not fit
std::string restart_line(ui::Ctx& c, float px, float width) {
    const unsigned all = restart_pending();
    std::string line;
    for (int keep = restart_state.n; keep >= 0; keep--) {   // (the first `keep` pending rows by name)
        unsigned shown = 0;
        int named = 0, more = 0;
        for (int i = 0; i < restart_state.n; i++)
            if ((all >> i) & 1) (named < keep ? (shown |= 1u << i, named++) : more++);
        line = "Restart needed: " + mr::names(restart_names, restart_state.n, shown);
        if (more) line += (named ? " and " : "") + std::to_string(more) + (named ? " more" : more > 1 ? " settings" : " setting");
        if (ui::size_of(c.f.strong, px, line.c_str()).x <= width) break;
    }
    return line;
}

/// (hookshot) The Debug tab's order: the tools' rows by their group (Row::cheat), Up and Down walking them so; Inventory item
/// and Change quantity are not shown (the Inventory page edits every amount)
struct DebugGroup { const char* title; int from, to; };   // (debug_tools_order[from, to))
// ((Triforce) Story & amiibo: the name pages' rows, under Player in the first column)
constexpr int debug_tools_order[] = {0, 1, 9, 10, 11, 12, 13, 8, 2, 3, 7, 4};
constexpr DebugGroup debug_groups[] = {{"Player", 0, 4}, {"Story & amiibo", 4, 7}, {"Travel", 7, 10}, {"Camera & world", 10, 12}};
constexpr int kDebugSecondColumn = 2;   // (the first group of the second column, when two fit)
constexpr int kDebugGroups = sizeof debug_groups / sizeof debug_groups[0];
constexpr int kDebugTools = sizeof debug_tools_order / sizeof debug_tools_order[0];
/// The Debug tab's rows as shown (its row numbers, tab_row_to_global's): the switch, the tools in debug_tools_order while they
/// are on, then the Developer rows; *dev where those start, *dev_head the Developer heading's row (-1 none). Their count
int debug_rows(int* out, int* dev, int* dev_head) {
    const int count = tab_row_count(TAB_DEBUG);
    int n = 0;
    *dev_head = -1;
    for (int k = 0; k < count; ++k)
        if (rows[tab_row_to_global(TAB_DEBUG, k)].value == &opts.debug_tools) out[n++] = k;
    for (int t = 0; t < kDebugTools; ++t)
        for (int k = 0; k < count; ++k)
            if (rows[tab_row_to_global(TAB_DEBUG, k)].cheat == debug_tools_order[t]) out[n++] = k;
    *dev = n;
    for (int k = 0; k < count; ++k) {
        const Row& r = rows[tab_row_to_global(TAB_DEBUG, k)];
        if (r.heading) *dev_head = k;
        else if (*dev_head >= 0 && r.cheat < 0 && r.value != &opts.debug_tools) out[n++] = k;
    }
    return n;
}
/// The Debug tab's row after sel (dir 1) or before it (dir -1), in debug_rows' order
int debug_step(int sel, int dir) {
    int order[64], dev = 0, head = -1;
    const int n = debug_rows(order, &dev, &head);
    int at = 0;
    while (at < n && order[at] != sel) at++;
    if (at == n) return order[0];
    return order[(at + n + dir) % n];
}
/// (hookshot) The Debug tab's rows from top down to bottom, sel selected: Debug tools across the top; the tools under their
/// headings, Player and Story & amiibo in the first column and Travel and Camera & world in the second when two fit (else
/// one under another);
/// the Developer rows across the foot. Each value cell a column's width, so their edges line up. The hits are the tab's row
/// numbers, as the list's: menu_tick's clicks and keys are the other tabs'. top to bottom is where whole rows stand (what
/// the scroll counts with); clip0 to clip1 is what shows of the column, as on the other tabs: a row scrolled past top or
/// bottom goes under the header or toward the guides, and is not cut off in the middle of the column
void draw_debug_rows(ui::Ctx& c, float cx0, float cx1, float top, float bottom, float clip0, float clip1, int sel) {
    const float s = c.s, px = 18 * s, pitch = 48 * s, rh = 40 * s, gap = 24 * s;   // (the other tabs' rows: 40 high, 48 apart)
    const float left = cx0 + 44 * s, right = cx1 - 44 * s, mid = (left + right) * .5f;
    int order[64], dev = 0, dev_head = -1;
    const int n = debug_rows(order, &dev, &dev_head);
    // (two columns only when each fits its longest name whole (name_cell's room, the selected's bold) and its value cells at
    // their narrowest; the cells then as wide as the room lets them, up to what they ask. (Triforce) In the Options screen's
    // column they do not: each half is 317 of the layout's units wide at any window, for the layout scales with the window's
    // height alone, and the columns ask 328 and 325 (forced, "Heart protection" and "Return to mark" are cut). So the tab is
    // one column everywhere, and the two columns are for a wider column or shorter names)
    const float halves_w = 2 * std::max(ui::size_of(c.f.strong, px, "Off").x, ui::size_of(c.f.strong, px, "On").x);
    auto need = [&](const Row& r, bool tight) {
        const float pad = (tight ? 36 : 64) * s;
        if (r.cheat == 9 && age_shown(9))
            return 2 * std::max(ui::size_of(c.f.strong, px, "Kid").x, ui::size_of(c.f.strong, px, "Adult").x) + pad;
        if (r.value && r.hi - r.lo > r.step) {   // (Triforce: a Developer row with more than Off and On: its widest value, and the arrows)
            float widest = 0;
            for (int v = r.lo; v <= r.hi; v += r.step) widest = std::max(widest, ui::size_of(c.f.strong, px, r.text(v)).x);
            return widest + (tight ? 56 : 68) * s;
        }
        if (r.cheat == 0 || r.cheat == 1 || r.cheat == 7 || r.value) return halves_w + pad;
        const std::string v = cheat_value(r.cheat);
        const char* const says[kCheatRows] = {"", "", "Mark", "Return", "", "", "", "", "Choose map", "Switch", "Edit items",
                                              "Open list", "Open list", "Choose file"};
        return ui::size_of(c.f.strong, px, v.empty() ? says[r.cheat] : v.c_str()).x + (cheat_chooses(r.cheat) ? 68 : tight ? 56 : 60) * s;   // (choice_cell keeps 48 for arrows either way)
    };
    float name_need[2] = {0, 0}, cell_full[2] = {halves_w + 64 * s, halves_w + 64 * s}, cell_tight[2] = {halves_w + 36 * s, halves_w + 36 * s};
    for (int i = 1; i < dev; ++i) {
        const Row& r = rows[tab_row_to_global(TAB_DEBUG, order[i])];
        const int col = i - 1 >= debug_groups[kDebugSecondColumn].from ? 1 : 0;   // (Player and Story & amiibo in the first, the rest in the second)
        name_need[col] = std::max(name_need[col], ui::size_of(c.f.strong, 18.5f * s, r.name).x + 40 * s);
        cell_full[col] = std::max(cell_full[col], need(r, false)), cell_tight[col] = std::max(cell_tight[col], need(r, true));
    }
    const float col_w = (right - left - gap) * .5f;
    // ((Triforce) the cells' widths, debug_tab_layout.h: a column's alike, and the switch's and the Developer rows', which span
    // the columns, alike: the widest any of those asks, for a row of more than two values asks more than Off and On)
    float span_need = 0;
    for (int i = 0; i < n; ++i)
        if (i == 0 || i >= dev) span_need = std::max(span_need, need(rows[tab_row_to_global(TAB_DEBUG, order[i])], false));
    const debug_tab::Widths widths = debug_tab::widths(col_w, right - left, name_need, cell_full, cell_tight, span_need);
    const bool two = widths.two;
    const float cell_w[2] = {widths.cell[0], widths.cell[1]}, span_w = widths.span;
    // (where each goes: a row number of the layout and a column)
    int vrow[64] = {}, vcol[64] = {}, head_row[kDebugGroups] = {}, head_col[kDebugGroups] = {};
    int next_row[2] = {1, 1};
    const bool tools = dev > 1;
    for (int g = 0; tools && g < kDebugGroups; ++g) {
        const int col = two && g >= kDebugSecondColumn ? 1 : 0;
        if (next_row[col] > 1) next_row[col]++;   // (a gap over a heading that follows another group)
        head_row[g] = next_row[col]++, head_col[g] = col;
        for (int t = debug_groups[g].from; t < debug_groups[g].to; ++t) vrow[1 + t] = next_row[col]++, vcol[1 + t] = col;
    }
    int foot = std::max(next_row[0], next_row[1]);
    if (tools) foot++;
    const int dev_row = foot;
    for (int i = dev; i < n; ++i) vrow[i] = dev_row + 1 + (i - dev);
    const int visual_count = n > dev ? dev_row + 1 + (n - dev) : foot;
    int sel_at = 0;
    for (int i = 0; i < n; ++i)
        if (order[i] == sel) sel_at = vrow[i];
    int shown = 0;
    const float first = scroll_rows(TAB_DEBUG, visual_count, sel_at, top, bottom, pitch, &shown);
    scroll_bar(c, cx0 + 20 * s, top, top + shown * pitch - (pitch - rh), visual_count, shown, first);
    auto heading = [&](float x0, float x1, int row, const char* title) {   // (as the list's Developer heading: gold, a rule after)
        const float y = top + (row - first) * pitch, ty = y + rh * 0.62f, tw = ui::size_of(c.f.heading, 19 * s, title).x;
        if (y + pitch <= clip0 || y >= clip1) return;
        ui::text_mid(c, c.f.heading, 19 * s, x0 + 2 * s, ty, ui::kGoldHi, title);
        if (x0 + tw + 16 * s < x1) c.d->AddLine(ImVec2(x0 + tw + 16 * s, ty), ImVec2(x1, ty), ui::alpha(ui::kEdge, 0.9f), std::max(1.0f, s));
    };
    c.d->PushClipRect(ImVec2(cx0, clip0), ImVec2(cx1, std::max(clip0, clip1)), true);
    for (int g = 0; tools && g < kDebugGroups; ++g)
        heading(head_col[g] ? mid + gap * .5f : left, !two || head_col[g] ? right : mid - gap * .5f, head_row[g], debug_groups[g].title);
    if (dev_head >= 0 && n > dev) heading(left, right, dev_row, rows[tab_row_to_global(TAB_DEBUG, dev_head)].name);
    const char* const buttons[kCheatRows] = {"", "", "Mark", "Return", "", "", "", "", "Choose map", "Switch", "Edit items",
                                             "Open list", "Open list", "Choose file"};
    for (int i = 0; i < n; ++i) {
        const int k = order[i];
        const Row& r = rows[tab_row_to_global(TAB_DEBUG, k)];
        const float y = top + (vrow[i] - first) * pitch;
        if (y + pitch <= clip0 || y >= clip1) continue;
        const bool in_tools = i > 0 && i < dev;
        const float x0 = two && in_tools && vcol[i] ? mid + gap * .5f : left;
        const float x1 = !two || !in_tools || vcol[i] ? right : mid - gap * .5f;
        const bool selected = k == sel;
        const float width = in_tools ? cell_w[vcol[i]] : span_w;
        const ImVec2 va(x1 - width, y), vb(x1, y + rh);
        name_cell(c, ImVec2(x0, y), ImVec2(va.x, y + rh), r.name, selected, restart_changed(r));   // (boxed, as on the other tabs)
        const float vm = r.value ? (r.hi - r.lo > r.step ? 0 : halves_meet(va, vb))   // ((Triforce) the halves' meeting, for the mouse)
                                 : r.cheat == 0 || r.cheat == 1 || r.cheat == 7 || age_shown(r.cheat) ? halves_meet(va, vb) : 0;
        if (r.value && r.hi - r.lo > r.step) {   // (Triforce: a Developer row with more than two values: a choice, as on the other tabs)
            const bool fixed = row_fixed(r.value);
            choice_cell(c, va, vb, r.text(*r.value), selected, !fixed, !fixed, fixed);
        } else if (r.value) {   // (Debug tools and the Developer rows: values of the options, Off and On)
            halves_cell(c, va, vb, std::string(r.text(r.hi)).c_str(), std::string(r.text(r.lo)).c_str(), *r.value == r.lo, selected,
                        row_fixed(r.value));
        } else if (r.cheat == 0 || r.cheat == 1 || r.cheat == 7) {
            halves_cell(c, va, vb, "On", "Off", cheat_value(r.cheat) != "On", selected, false);
        } else if (age_shown(r.cheat)) {   // (Link's age now: the other half switches)
            halves_cell(c, va, vb, "Kid", "Adult", link_age.load(std::memory_order_relaxed) == 1, selected, false);
        } else {
            const std::string v = cheat_value(r.cheat);
            choice_cell(c, va, vb, v.empty() ? buttons[r.cheat] : v, selected, cheat_chooses(r.cheat), cheat_chooses(r.cheat), r.cheat == 9);
        }
        hits.add(x0, std::max(y, clip0), x1, std::min(y + rh, clip1), menu_hit::kRow, k, va.x, vb.x, 0, 0, 0, vm);
    }
    c.d->PopClipRect();
}

/// The tabs across the header (as the pause menu's categories: MenuOverlay_00, 460 units apart, L and R at their ends; closer
/// with more than five). sel: the one shown, -1 the game's own. game: "Game" in front of ours, inside the game's Options
/// screen. hit: the mouse's places noted
void draw_tabs(ui::Ctx& c, int sel, bool game, bool hit) {
    static const char* const ours[TAB_COUNT] = {"Display", "Graphics", "Audio", "Controls", "System", "Debug"};
    const float s = c.s, hh = banner_h(c);
    // (Triforce) the strip's entries in their order: the game's own (-1), ours to System, Debug when shown (Mods is not one:
    // it is an entry of its own under the game's System list and the title's, kModsRowPage)
    int entry[TAB_COUNT + 2], tabs = 0;
    if (game) entry[tabs++] = -1;
    for (int t = 0; t < TAB_DEBUG; t++) entry[tabs++] = t;
    if (debug_shown()) entry[tabs++] = TAB_DEBUG;
    const float ty = ui::at(c, 0, 998).y, step = ui::lu(c, tabs > 7 ? 320 : tabs > 6 ? 350 : tabs > 5 ? 400 : 460), tpx = ui::lu(c, 54 * ui::kType);
    const float tx0 = c.w * 0.5f - step * (tabs - 1) * 0.5f;
    for (int i = 0; i < tabs; i++) {
        const int tab = entry[i];   // (-1: the game's own)
        const float x = tx0 + i * step;
        const bool shown = tab == sel;
        if (shown) {   // (the chosen category: the pause menu's own plate behind its name, MenuHeaderSelect: PaMenuCategory's
            // 540 x 260 pane, 34 units under the header's middle line, in the brown the pause menu's System tab has)
            const ImVec2 m = ui::at(c, 0, 966);
            const float hw = std::min(ui::lu(c, 270), step * 0.5f + ui::lu(c, 40));
            if (const ui::Art* t = ui::art("MenuHeaderSelect_00")) ui::image(c, t, ImVec2(x - hw, m.y - ui::lu(c, 130)), ImVec2(x + hw, m.y + ui::lu(c, 130)), ui::rgba(190, 125, 62));
            else c.d->AddRectFilled(ImVec2(x - step * 0.5f, 0), ImVec2(x + step * 0.5f, hh + ui::lu(c, 6)), ui::kTabInset);
        } else if (hit && tab >= 0 && tab == hover_tab) {
            c.d->AddRectFilled(ImVec2(x - step * 0.5f + 4 * s, ty - ui::lu(c, 60)), ImVec2(x + step * 0.5f - 4 * s, ty + ui::lu(c, 60)), ui::rgba(60, 42, 22, 70), 6 * s);
        }
        ui::text_mid_centre(c, c.f.strong, tpx, x, ty, ui::kInk, tab < 0 ? "Game" : tab == kModsTab ? "Mods" : ours[tab]);
        if (hit) hits.add(x - step * 0.5f, 0, x + step * 0.5f, hh, menu_hit::kTab, tab < 0 ? kGameTab : tab);
    }
    for (int i = 0; i < 2; i++) {   // (L and R at the tabs' ends, the game's icons: MenuOverlay_00's T_BtnL and T_BtnR, 90 units)
        const float x = i ? tx0 + (tabs - 1) * step + ui::lu(c, 298) : tx0 - ui::lu(c, 298);
        if (c.pad && c.f.glyph) {
            const char* code = i ? ui::kIconR : ui::kIconL;
            const ImVec2 sz = ui::size_of(c.f.glyph, ui::lu(c, 90), code);
            c.d->AddText(c.f.glyph, ui::lu(c, 90), ImVec2(x - sz.x * 0.5f, ty - sz.y * 0.5f + ui::lu(c, 2)), ui::kInk, code);
        } else {   // (the keys: Q and E; inside the game's Options the keys bound to the Switch's L and R, which are its own there)
            std::string label = c.pad ? (i ? "R" : "L") : (i ? "E" : "Q");
            if (!c.pad && game && can_bind())
                if (const int vk = host->binding(HOS_BIND_KEY, i ? HOS_SW_R : HOS_SW_L, 0)) label = key_name(vk);
            const float kh = ui::lu(c, 62), kw = std::max(ui::lu(c, 62), ui::size_of(c.f.strong, ui::lu(c, 40), label.c_str()).x + ui::lu(c, 30));
            c.d->AddRectFilled(ImVec2(x - kw * 0.5f, ty - kh * 0.5f), ImVec2(x + kw * 0.5f, ty + kh * 0.5f), ui::kInk, ui::lu(c, 14));
            ui::text_mid_centre(c, c.f.strong, ui::lu(c, 40), x, ty, ui::rgba(40, 30, 20), label.c_str());
        }
        if (hit) hits.add(i ? x - ui::lu(c, 90) : 0, 0, i ? c.w : x + ui::lu(c, 90), hh, i ? menu_hit::kTabNext : menu_hit::kTabPrev, -1);
    }
}

/// The Options screen: the header with the tabs across it (as the pause menu's categories: MenuOverlay_00, 460 units apart,
/// L and R at their ends), the rows in the list's column (SystemOption_00 and its rows' layouts: a row 1836 x 136 units, 170
/// apart, its name 840 left of the list's middle, its value's plate 820 x 116 at 500 right of it, the cursor round the
/// value), the caption plate at the right with what the selected row is, the guides at the foot
void draw_options(ui::Ctx& c) {
    const float s = c.s, hh = banner_h(c);
    ui::banner(c, hh);
    draw_tabs(c, cur_tab == TAB_MOTION ? TAB_CONTROLS : cur_tab, embed_open, true);   // (the Motion page: under Controls)
    // the rows, in the column; the caption beside it
    float cx0, cx1, hx0, hx1;
    bool narrow;
    columns(c, &cx0, &cx1, &hx0, &hx1, &narrow);
    const int count = tab_row_count(cur_tab), sel = tab_selected[cur_tab];
    const Row& selr = rows[tab_row_to_global(cur_tab, sel)];
    const char* hint = selr.value && selr.hint ? selr.hint(*selr.value, -1) : nullptr;
    const std::string share_hint = share_row(selr) ? share_line(share_row(selr)) : std::string();   // (Triforce: what the host did)
    if (!share_hint.empty()) hint = share_hint.c_str();
    const int sel_cheat = mod_cheat_slot(selr.name);   // (Triforce: a mod's cheat: the mod it is from, or why it cannot be switched)
    if (sel_cheat >= 0 && mod_cheats.line(sel_cheat)[0]) hint = mod_cheats.line(sel_cheat);
    const float cap_w = hx1 - hx0 - 2 * ui::lu(c, 76);
    const std::string pending_line = restart_pending() ? restart_line(c, kHintPt * s, cap_w - 20 * s) : "";
    static float help_tall = 0, help_tall_w = 0;
    if (help_tall_w != cap_w) help_tall_w = cap_w, help_tall = measure_help(c, cap_w);
    // (the guides: what the buttons do on this row, as the game's own say only that)
    const bool confirms = &selr == &rows[kRestartRow] || &selr == &rows[kQuitRow];
    ui::Prompt prompt_list[5] = {{ui::kLeftRight, "Change"}, {ui::kTabs, "Category"}, {ui::kBack, "Back"}};
    int prompts_n = 3;
    if (!selr.value) prompt_list[0] = {ui::kConfirm, confirms ? (pending == &selr ? "Confirm" : selr.name) : "Open"};
    if (share_row(selr)) prompt_list[0] = {ui::kConfirm, share_label(share_row(selr))};
    if (sel_cheat >= 0) prompt_list[0] = {ui::kLeftRight, "Change"};   // (Triforce: a mod's cheat, On | Off)
    if (cur_tab == TAB_MOTION) prompt_list[1] = {ui::kBack, "Back"}, prompts_n = 2;   // (Esc: to Controls)
    if (selr.cheat >= 0) {   // (a debug tool: Change when Left and Right do something, then what confirm does)
        prompts_n = 0;
        if (selr.cheat <= 1 || selr.cheat == 7 || cheat_chooses(selr.cheat) || age_shown(selr.cheat))
            prompt_list[prompts_n++] = {ui::kLeftRight, "Change"};
        if (const char* says = cheat_confirm(selr.cheat)) prompt_list[prompts_n++] = {ui::kConfirm, says};
        prompt_list[prompts_n++] = {ui::kTabs, "Category"}, prompt_list[prompts_n++] = {ui::kBack, "Back"};
    }
    const float bar_top = ui::prompt_bar_top(c);   // (the strip itself drawn last, over all: below)
    const bool after = restart_changed(selr);
    caption(c, hx0, hx1, nullptr, selr.help, hint, pending_line.empty() ? nullptr : pending_line.c_str(), help_tall, after);
    // (measured on the game's screen: a row's plate 1824 x 124, 170 apart, the first one's top 344 under the screen's top; the
    // value's near-black panel 804 x 108, its middle 500 right of the list's)
    const float rx = ui::at(c, kListX, 0).x, half = ui::lu(c, 912), rx0 = rx - half, rx1 = rx + half;
    const float rh = ui::lu(c, 124), pitch = ui::lu(c, 170), top = ui::at(c, 0, 736).y, bottom = ui::at(c, 0, -790).y;
    const float vx = rx + ui::lu(c, 500), vhw = ui::lu(c, 402), vhh = ui::lu(c, 54);
    const float clip0 = hh - ui::lu(c, 14), clip1 = bar_top + ui::lu(c, 50);
    if (cur_tab == TAB_DEBUG) {   // (the Debug tab: its own layout of the same rows)
        marquee_max = 0;
        draw_debug_rows(c, cx0, cx1, top, bottom, clip0, clip1, sel);
        ui::prompt_bar(c, prompt_list, prompts_n);
        return;
    }
    int shown = 0;
    const float first = scroll_rows(cur_tab, count, sel, top, bottom, pitch, &shown);
    scroll_bar(c, ui::at(c, kListX - 1102, 0).x, ui::at(c, 0, 100).y, ui::at(c, 0, -212).y, count, shown, first);   // (PaScrollBar: 56 under the middle)
    marquee_max = 0;   // (set again below while the selected row's value is too long)
    c.d->PushClipRect(ImVec2(cx0, clip0), ImVec2(cx1, clip1), true);
    for (int i = 0; i < count; i++) {
        const float y = top + (i - first) * pitch, my = y + rh * 0.5f;
        if (y + rh < clip0 || y > clip1) continue;
        const Row& r = rows[tab_row_to_global(cur_tab, i)];
        if (r.heading) {   // (a section: its name in gold, a rule after it, as the Debug tab's headings)
            // ((Triforce) 11-17, the tabs in sections: the game's plain separator had no name on it)
            const float hx = rx0 + ui::lu(c, 24), hpx = 19 * s, tw = ui::size_of(c.f.heading, hpx, r.name).x;
            ui::text_mid(c, c.f.heading, hpx, hx, my, ui::kGoldHi, r.name);
            if (hx + tw + 16 * s < rx1) c.d->AddLine(ImVec2(hx + tw + 16 * s, my), ImVec2(rx1, my), ui::alpha(ui::kEdge, 0.9f), std::max(1.0f, s));
            continue;
        }
        const bool selected = i == sel;
        name_cell(c, ImVec2(rx0, y), ImVec2(rx1, y + rh), r.name, selected, restart_changed(r));
        const ImVec2 va(vx - vhw, my - vhh), vb(vx + vhw, my + vhh);
        float t0 = 0, t1 = 0, vm = 0;   // ((Triforce) vm: where a toggle's halves meet, for the mouse)
        if (const int cheat = mod_cheat_slot(r.name); cheat >= 0) {   // (Triforce: a mod's cheat: On | Off as the toggles, or why not)
            if (mod_cheats.fixed(cheat)) choice_cell(c, va, vb, mod_cheats.fixed_text(cheat), selected, false, false, true);
            else {
                halves_cell(c, va, vb, "On", "Off", !mod_cheats.is_on(cheat), selected, false);
                vm = halves_meet(va, vb);
            }
        } else if (!r.value) {   // (a page, or Restart and Quit: Confirm until the second press)
            const bool ends = &r == &rows[kRestartRow] || &r == &rows[kQuitRow];
            if (pending == &r) choice_cell(c, va, vb, "Press again", selected, false, false);
            else choice_cell(c, va, vb, ends ? r.name : share_row(r) ? share_label(share_row(r)) : "Open", selected, false, true, false, true);
        } else {
            const bool fixed = row_fixed(r.value);
            if (is_slider(r)) {
                slider_cell(c, va, vb, r.text(*r.value), menu_hit::slider_permille(*r.value, r.lo, r.hi), selected, &t0, &t1);
                t0 = t1 = 0;   // (no track to drag: the arrows step it)
            } else if (r.text == tx_onoff && !fixed) {   // (a toggle as the game's: On at the left, Off at the right)
                halves_cell(c, va, vb, "On", "Off", *r.value == 0, selected, fixed);
                vm = halves_meet(va, vb);
            } else {
                const bool output_row = r.value == &opts.output_res;
                const bool left = !fixed && (!output_row || step_output_choice(*r.value, -1) != *r.value) && (!stops_at_ends(r) || *r.value > r.lo);
                const bool right = !fixed && (!output_row || step_output_choice(*r.value, 1) != *r.value) && (!stops_at_ends(r) || *r.value < r.hi);
                choice_cell(c, va, vb, r.text(*r.value), selected, left, right, fixed);
            }
        }
        hits.add(rx0, std::max(y, clip0), rx1, std::min(y + rh, clip1), menu_hit::kRow, i, va.x, vb.x, t0, t1, 0, vm);
    }
    c.d->PopClipRect();
    if (cur_tab == TAB_SYSTEM) {   // (which build, so a report can say it without opening the log)
        const std::string ver = std::string("Triforce ") + HOOKSHOT_VERSION;
        ui::text_mid(c, c.f.body, ui::lu(c, 36), ui::at(c, -1840, 0).x, ui::at(c, 0, -1030).y, ui::rgba(255, 251, 238, 120), ver.c_str());
    }
    ui::prompt_bar(c, prompt_list, prompts_n);   // (the guides, on top of the caption and the rows)
}

void draw_controls(ui::Ctx& c) {
    // (the page's own rows first, kTop of them (the layout preset, the button prompts), then a row per control and Reset to
    // defaults: one list, csel its selection)
    const float s = c.s, hh = title_banner(c, "Controls", nullptr);
    float cx0, cx1, hx0, hx1;
    bool narrow;
    columns(c, &cx0, &cx1, &hx0, &hx1, &narrow);
    column(c, cx0, cx1, hh);
    const int n = kTop + kControls + 1, sel_i = csel - kTop;   // (sel_i: the control's, kControls Reset; negative: a page row)
    const ui::Prompt idle[] = {{ui::kUpDown, "Choose"}, {ui::kLeftRight, "Column"}, {ui::kConfirm, "Bind"}, {ui::kKey, "Clear", "Del"},
                               {ui::kBack, "Back"}};
    const ui::Prompt option[] = {{ui::kUpDown, "Choose"}, {ui::kLeftRight, "Change"}, {ui::kBack, "Back"}};
    const ui::Prompt wait[] = {{ui::kBack, "Cancel"}};
    const float bar_top = ui::prompt_bar_top(c);   // (the strip itself drawn last, over all: below)
    const Row* page_row = sel_i < 0 ? &rows[tab_row_to_global(TAB_PAGE, csel)] : nullptr;
    const char* title = page_row ? page_row->name : sel_i < kControls ? controls[sel_i].name : "Reset to defaults";
    const char* help = listening ? (listening == 1 ? "Press the gamepad button to use. Esc cancels." : "Press the key to use. Esc cancels.")
                       : page_row ? page_row->help
                                  : "Which gamepad button and which keys press each of the Switch's buttons. Enter binds the one "
                                    "selected; Delete clears it. The game's button icons follow after a restart.";
    const char* hint = page_row ? (page_row->hint ? page_row->hint(*page_row->value, -1) : nullptr)
                                : "Gamepad: the D-pad moves, A binds, B goes back.";
    const float bottom = bar_top - 6 * s;   // (the rows end above the guides)
    caption(c, hx0, hx1, title, help, hint, nullptr, 0, page_row && restart_changed(*page_row));
    // (the options' rows, a little lower: 24 controls. A plate across the row, its name at the left, the gamepad's input and the
    // keys on value plates at the right)
    const float rxm = ui::at(c, kListX, 0).x, rx0 = rxm - ui::lu(c, 912), rx1 = rxm + ui::lu(c, 912);
    const float pad_x = rx0 + (rx1 - rx0) * 0.34f, key_x = rx0 + (rx1 - rx0) * 0.67f;
    float top = hh + 12 * s;
    ui::text_mid_centre(c, c.f.body, ui::lu(c, 40), (pad_x + key_x) * 0.5f, top + 9 * s, ui::kSoft, "Gamepad");
    ui::text_mid_centre(c, c.f.body, ui::lu(c, 40), (key_x + rx1) * 0.5f - ui::lu(c, 8), top + 9 * s, ui::kSoft, "Keyboard");
    top += 24 * s;
    const float rh = ui::lu(c, 112), pitch = ui::lu(c, 140), vin = ui::lu(c, 8);
    int shown = 0;
    const float first = scroll_rows(kControlsSlot, n, csel, top, bottom, pitch, &shown);
    scroll_bar(c, cx0 + 20 * s, top, top + shown * pitch - (pitch - rh), n, shown, first);
    c.d->PushClipRect(ImVec2(cx0, top - 8 * s), ImVec2(cx1, bottom + 8 * s));
    for (int j = 0; j < n; j++) {
        const float y = top + (j - first) * pitch;
        if (y + rh < top - 8 * s || y > bottom + 8 * s) continue;
        const bool sel = j == csel;
        const int i = j - kTop;
        const float y0 = std::max(y, top - 8 * s), y1 = std::min(y + rh, bottom + 8 * s);
        if (i < 0) {   // (one of the page's own rows: a name and a choice, as on the tabs)
            const Row& r = rows[tab_row_to_global(TAB_PAGE, j)];
            // (their names wider than a control's, as wide as the tabs' rows': "Button prompts" whole at any size)
            const float opt_x = rx0 + (rx1 - rx0) * 0.5f;
            name_cell(c, ImVec2(rx0, y), ImVec2(rx1, y + rh), r.name, sel, restart_changed(r), 17);
            choice_cell(c, ImVec2(opt_x, y + vin), ImVec2(rx1 - vin, y + rh - vin), r.text(*r.value), sel, *r.value > r.lo, *r.value < r.hi);
            hits.add(rx0, y0, rx1, y1, menu_hit::kCtlOption, j, opt_x, rx1);
            continue;
        }
        if (i == kControls) {
            ui::button(c, ImVec2(rx0, y + ui::lu(c, 10)), ImVec2(pad_x + ui::lu(c, 120), y + rh + ui::lu(c, 10)), "Reset to defaults", sel);
            hits.add(rx0, y0, pad_x + ui::lu(c, 120), y1 + ui::lu(c, 10), menu_hit::kCtlRow, i);
            continue;
        }
        const Control& ctl = controls[i];
        name_cell(c, ImVec2(rx0, y), ImVec2(rx1, y + rh), ctl.name, sel, false, 17);
        hits.add(rx0, y0, pad_x, y1, menu_hit::kCtlRow, i);
        for (int col = 0; col < 2; col++) {   // (two value cells, a gap between them, each drawn as the tabs' values are)
            const float a = col ? key_x + 3 * s : pad_x, b = col ? rx1 - vin : key_x - 3 * s;
            std::string v = col == 0 ? (keys_only(i) ? "-" : pad_name(host->binding(HOS_BIND_PAD, ctl.button, 0))) : keys_text(ctl.button);
            const bool here = sel && col == ccol;
            const ImVec2 va(a, y + vin), vb(b, y + rh - vin);
            if (here && listening) {   // (waiting for the new input: the light plate, the cursor round it)
                ui::value_plate(c, va, vb);
                ui::text_mid_centre(c, c.f.body, ui::lu(c, 50), (a + b) * 0.5f, y + rh * 0.5f - ui::lu(c, 2), ui::kInk,
                                    col == 0 ? "Press a button..." : "Press a key...");
                ui::cursor(c, va - ImVec2(ui::lu(c, 2.5f), ui::lu(c, 3)), vb + ImVec2(ui::lu(c, 2.5f), ui::lu(c, 3)));
            } else {
                choice_cell(c, va, vb, v, here, false, false, col == 0 && keys_only(i));
            }
            if (!(col == 0 && keys_only(i))) hits.add(a, y0, b, y1, menu_hit::kCtlCell, i, 0, 0, 0, 0, col);
        }
    }
    c.d->PopClipRect();
    (void)(listening ? ui::prompt_bar(c, wait, 1) : sel_i < 0 ? ui::prompt_bar(c, option, 3) : ui::prompt_bar(c, idle, 5));   // (the prompts, on top of the help and the rows)
}

// ---- (Triforce) The Debug tab's name pages: Game flags, Cutscenes and Scan amiibo (page 6) ----
// After NX2navi's F9 panel (its flags by name and its cutscene starter) and its amiibo page, in this menu's own form; none
// of that code is here. One page for the three: a list of names in groups (debug_names.h), the cards of the group shown,
// the chosen one's action. The flags' and the events' names are read from the player's own game files, in memory, on a
// thread of its own the first time one of the two pages opens (ui::game_files); the amiibo page lists the game folder's
// amiibo folder each time it opens. What a page wants goes to the game's side as lines (oot_debug_link.h): the flags' and
// the events' through HOS_OPT_DEBUG_ASK, read at Link's update (host/hos_main.c), a figure's file through
// HOS_OPT_AMIIBO_ASK, read at the pad read (host/nn_nfp.c); what the game's side says comes back through
// menu_option_set. The game's side reads only while the game runs: a change asked for inside the game's paused Options
// screen is made when the game goes on. Nothing is read, listed or sent before a page is opened.
enum { kNamesFlags, kNamesEvents, kNamesAmiibo, kNamesKinds };
namespace dn = debug_names;
int names_kind = kNamesFlags;
dn::Page names_page[kNamesKinds];
bool names_edit = false;   // (a flag's On and Off are chosen with Left and Right)
std::mutex names_mu;       // (the scan's lists and the lines either way: the scan's thread and the host's threads use them too)
std::atomic<int> names_scan{0};   // (0 not started, 1 the game's files are being read, 2 read, 3 the game's files were not found)
dn::List names_scanned[2];        // (the flags' and the events', until the pages take them)
std::atomic<bool> names_new{false};
dn::Lines names_ask, amiibo_ask;  // (HOS_OPT_DEBUG_ASK's and HOS_OPT_AMIIBO_ASK's lines)
dn::States names_states;          // (the flags' states for the cards shown)
bool names_watching = false;      // (the game's side was asked for some flags' states)
std::string names_in;             // (the line the game's side is sending, a byte a call)
std::string names_status;         // (what became of the last flag or event asked for)
std::string names_status_of;      // (the flag or the event that was: its line shows while that name is the chosen one)
int names_status_kind = -1;       // (and which: a flag and an event may have the same name)
std::atomic<bool> names_asked{false};   // (a flag or an event was asked for since the Debug tools were last seen off)
std::atomic<bool> names_status_new{false};
std::atomic<int> amiibo_said{HOS_AMIIBO_NONE};   // (what became of the figure: HOS_AMIIBO_*)
std::atomic<bool> amiibo_new{false};
std::string amiibo_chosen;        // (the file last sent, "" none)
/// What the Scan amiibo page says became of the figure (short: the line at the foot of the screen while the options are closed)
const char* amiibo_text(int said, bool brief) {
    switch (said) {
    case HOS_AMIIBO_WAITING: return brief ? "amiibo: waiting for the game to look for it" : "Chosen. It is on the reader as soon as the game looks for a figure.";
    case HOS_AMIIBO_ON_READER: return brief ? "amiibo: the game found the figure" : "The game found the figure and reads it.";
    case HOS_AMIIBO_READ: return brief ? "amiibo: read by the game" : "The game has read the figure. Choose it again for another touch.";
    case HOS_AMIIBO_REFUSED: return brief ? "amiibo: not a figure's dump" : "That file is not a figure's dump (532, 540 or 572 bytes, as a tag has them).";
    case HOS_AMIIBO_NO_FILE: return brief ? "amiibo: the file could not be read" : "The file could not be read.";
    }
    return nullptr;
}
/// What a name page is, for its caption
const char* names_help(int kind) {
    static const char* const help[kNamesKinds] = {
        "The game's flags by name, as your game files have them. Confirm, then Left sets the chosen flag and Right clears it. The game "
        "takes a change when it runs.",
        "The game's events by name, as your game files have them. Start asks the game for the chosen one as one of its own actors "
        "would. It starts when the game runs. Save first.",
        "Dumps of your own figures. Scan puts the chosen one on the reader when the game looks for a figure. Only the figure's serial "
        "number and which figure it is are read from the file: no key is needed."};
    return help[kind];
}
/// The flags' and the events' names from the player's game files, on a thread of its own (once a run): the Bools' hashes of
/// the GameDataList (the last by name: the highest version), then every event flow, for the flags' names in it and whether
/// it is an event. In memory only; the log gets the counts
void names_scan_thread() {
    std::vector<uint32_t> bools;
    std::vector<std::string> flags, events;
    const int64_t t0 = prof_now();
    bool found = ui::game_files("GameData", ".byml.zs", [&](const std::string& name, const std::vector<unsigned char>& bytes) {
        std::vector<uint32_t> got;
        if (!name.compare(0, 13, "GameDataList.") && dn::gamedata_bools(bytes, got)) bools = std::move(got);
    });
    found = found && ui::game_files("Event/EventFlow", ".bfevfl.zs", [&](const std::string& name, const std::vector<unsigned char>& bytes) {
        if (dn::event_has_script(bytes)) events.push_back(dn::event_of_file(name));
        dn::flag_names(bytes, bools, flags);
    });
    dn::List flag_list = dn::make_list(std::move(flags)), event_list = dn::make_list(std::move(events));
    logf("menu: the Debug tab's names, from the game's files: %d flags' names for %d Bools, %d events (%.0f ms)%s", (int)flag_list.names.size(),
         (int)bools.size(), (int)event_list.names.size(), (prof_now() - t0) / 1e6, found ? "" : "; the game's files were not found");
    {
        std::lock_guard lk(names_mu);
        names_scanned[0] = std::move(flag_list), names_scanned[1] = std::move(event_list);
    }
    names_scan.store(found ? 2 : 3, std::memory_order_release);
    names_new.store(true, std::memory_order_release);
}
/// A name page opened from its row on the Debug tab
void names_open(int kind) {
    names_kind = kind, names_edit = false, page = 6;
    if (kind == kNamesAmiibo) {   // (the folder as it is now: a dump put there since shows without a restart)
        char exe[MAX_PATH];
        const DWORD n = game_module_path(exe, MAX_PATH);
        const std::string dir(exe, n);
        names_page[kind].set_list(dn::make_list(ui::folder_files(dir.substr(0, dir.find_last_of("\\/") + 1) + "amiibo", ".bin"), false, "Your figures"));
    } else {
        int none = 0;
        if (names_scan.compare_exchange_strong(none, 1)) std::thread(names_scan_thread).detach();
    }
}
/// The chosen flag asked to be set or cleared. Once: Left or Right held repeats the key, and the same request is not queued
/// again behind itself
void names_send_flag(bool on) {
    const dn::Page& p = names_page[kNamesFlags];
    char line[32];
    if (p.empty() || !oot_ask_flag(line, sizeof line, p.list.hashes[p.choice], on)) return;
    std::lock_guard lk(names_mu);
    if (!names_ask.repeats(line)) names_ask.push(line);
    names_asked.store(true, std::memory_order_relaxed);
    names_status = on ? "Asked to set it: done when the game runs." : "Asked to clear it: done when the game runs.";
    names_status_of = p.list.names[p.choice], names_status_kind = kNamesFlags;
}
/// Confirm on the chosen name: a flag's On and Off are chosen next; an event is asked for, and the options close (it starts
/// when the game goes on); a figure's file is sent, or the figure taken away when it is the one that waits or is being read
void names_confirm(bool& changed) {
    dn::Page& p = names_page[names_kind];
    const std::string& name = p.list.names[p.choice];
    changed = true;
    if (names_kind == kNamesFlags) {
        names_edit = true;
        return;
    }
    std::lock_guard lk(names_mu);
    if (names_kind == kNamesEvents) {
        char line[OOT_LINE_MAX + 1];
        if (oot_ask_event(line, sizeof line, name.c_str())) names_ask.push(line);
        names_asked.store(true, std::memory_order_relaxed);
        names_status = "Asked for: it starts when the game runs.";
        names_status_of = name, names_status_kind = kNamesEvents;
        open_now = false, page = 0;
        return;
    }
    const int said = amiibo_said.load(std::memory_order_relaxed);
    const bool away = (said == HOS_AMIIBO_WAITING || said == HOS_AMIIBO_ON_READER) && amiibo_chosen == name;
    amiibo_chosen = away ? std::string() : name;
    if (amiibo_ask.waiting.size() > (amiibo_ask.at ? 1u : 0u)) amiibo_ask.waiting.resize(amiibo_ask.at ? 1 : 0);   // (the newest choice alone)
    amiibo_ask.push(amiibo_chosen);
}
/// (Triforce) the mouse on a name page: a card is chosen by a click, the action button acts, the group's and the page's
/// arrows turn them, a flag's halves set and clear it; the wheel turns the page shown and leaves the choice; a right click
/// goes back
void names_mouse(const MkwVkPointer& ptr, int mx, int my, bool* press, bool& changed) {
    dn::Page& p = names_page[names_kind];
    const menu_hit::Hit hit = hits.at((float)mx, (float)my);
    if (ptr.pressed & HOS_MOUSE_LEFT) {
        if (hit.part == menu_hit::kListLine && p.choose(hit.index)) names_edit = false, changed = true;
        else if (hit.part == menu_hit::kButton && hit.index == 0) press[7] = true;
        else if (hit.part == menu_hit::kButton && hit.index == 1 && hit.side) press[hit.side < 0 ? 3 : 4] = true;
        else if (hit.part == menu_hit::kButton && (hit.index == 2 || hit.index == 3)) press[hit.index == 2 ? 13 : 14] = true, names_edit = false;
        else if (hit.part == menu_hit::kButton && hit.index == 4 && hit.side) p.turn_group(hit.side), names_edit = false, changed = true;
    }
    if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;
    if (ptr.wheel) p.turn_shown(-ptr.wheel), changed = true;
}
/// (Triforce) the keys on a name page: back leaves a flag's On and Off first, then the page (to the Debug tab, its row still
/// selected); L and R another group; the arrows, Page Up and Down, Home and End move the choice; confirm acts
void names_press(const bool* press, bool tab_left, bool tab_right, bool& changed) {
    dn::Page& p = names_page[names_kind];
    if (press[5] && names_edit) names_edit = false, changed = true;
    else if (press[5]) page = 0, changed = true;
    else if (p.empty()) return;
    else if (tab_left || tab_right) p.turn_group(tab_left ? -1 : 1), names_edit = false, changed = true;
    else if (names_edit) {   // (a flag: Left sets it, Right clears it, as its halves are drawn)
        if (press[7]) names_edit = false, changed = true;
        else if (press[3] || press[4]) names_send_flag(press[3]), changed = true;
    } else {
        const int cards = press[4] - press[3], card_rows = press[2] - press[1], pages = press[14] - press[13];
        if (cards || card_rows || pages || press[15] || press[16]) p.move(cards, card_rows, pages, press[15], press[16]), changed = true;
        else if (press[7]) names_confirm(changed);
    }
}
/// (Triforce) Each tick: the lists the scan made are taken; the game's side is asked for the states of the flags shown (and
/// told when none is shown any more); and what it said while the options are closed is the line at the foot of the screen
void names_tick() {
    if (names_new.exchange(false, std::memory_order_acquire)) {
        std::lock_guard lk(names_mu);
        names_page[kNamesFlags].set_list(std::move(names_scanned[0]));
        names_page[kNamesEvents].set_list(std::move(names_scanned[1]));
        names_edit = false;   // (another list under the choice: not in the middle of a flag's On and Off)
    }
    // (the Debug tools turned off: what was asked of the game and not yet taken by it is not done after all)
    if (!cheats_on() && names_asked.exchange(false, std::memory_order_relaxed)) {
        std::lock_guard lk(names_mu);
        names_ask.drop_but('W');
    }
    const bool open = open_now.load(), shows_flags = open && page == 6 && names_kind == kNamesFlags;
    if (shows_flags || names_watching) {
        const std::vector<uint32_t> shown = shows_flags ? names_page[kNamesFlags].watched() : std::vector<uint32_t>();
        char line[OOT_LINE_MAX + 1];
        std::lock_guard lk(names_mu);
        if (names_states.ask(shown) && oot_ask_watch(line, sizeof line, shown.data(), (int)shown.size())) names_ask.push(line, 'W');
        names_watching = !shown.empty();
    }
    if (!open || page != 6) names_edit = false;
    const bool said = names_status_new.exchange(false, std::memory_order_relaxed), figure = amiibo_new.exchange(false, std::memory_order_relaxed);
    if (open || (!said && !figure)) return;
    const char* text = figure ? amiibo_text(amiibo_said.load(std::memory_order_relaxed), true) : nullptr;
    std::lock_guard lk(names_mu);
    if (!text && !said) return;
    snprintf(notice_text, sizeof notice_text, "%s", text ? text : names_status.c_str());
    notice_until = prof_now() + 2500000000LL;
}

/// A small square page arrow at a (its size side): dir 0 back, 1 on; dim when it cannot go that way
void page_arrow(ui::Ctx& c, ImVec2 a, float side, int dir, bool can) {
    const float s = c.s;
    const ImVec2 b = a + ImVec2(side, side);
    c.d->AddRectFilled(a, b, can ? ui::kCellName : ui::alpha(ui::kCellName, 0.4f), 3 * s);
    c.d->AddRect(a, b, can ? ui::kEdge : ui::alpha(ui::kEdge, 0.4f), 3 * s, std::max(1.0f, s));
    ui::triangle(c, (a + b) * 0.5f, 10 * s, dir, can ? ui::kInkBright : ui::kDim);
}

/// A tag at the right end of a card (its right edge x, its middle y): the text on an accent plate; its width
float card_tag(ui::Ctx& c, float x, float y, const char* tag, bool draw = true) {
    const float s = c.s, tw = ui::size_of(c.f.strong, 13 * s, tag).x + 14 * s;
    if (draw) {
        const ImVec2 g(x - tw, y - 10 * s);
        c.d->AddRectFilled(g, g + ImVec2(tw, 20 * s), ui::kAccent, 2 * s);
        ui::text_mid(c, c.f.strong, 13 * s, g.x + 7 * s, y, ui::kGoldInk, tag);
    }
    return tw;
}
/// (Triforce) A card's second line when it says "not": Locked, Quantity 0, Not in this save. Quieter than a line that says
/// "has", and still read at a glance: ui::kDim, which it was, is the plate's own tone and could not be read on it
const ImU32 kCardOff = ui::alpha(ui::kSoft, 0.62f);

void draw_inventory(ui::Ctx& c) {
    const float s = c.s;
    const int begin = inventory_begin(), end = inventory_end();
    bool loaded = false;   // (a save's counts reported: -1 for all until the game side has a save to read)
    for (int i = 0; i < kItems && !loaded; ++i) loaded = item_counts[i].load(std::memory_order_relaxed) >= 0;
    char count_text[48];
    snprintf(count_text, sizeof count_text, "%d items", end - begin);
    const float hh = title_banner(c, "Inventory", count_text);
    // ((Triforce) the cards and the amount show the change asked and not yet taken by the game: in its paused Options screen
    // it is taken when the game goes on, and a press that changed nothing on the screen read as one that did nothing)
    const int pending = cheat_action.load(std::memory_order_relaxed);
    const int have = debug_pending::shown(pending, item_choice, item_counts[item_choice].load(std::memory_order_relaxed));
    const char* edit = item_kinds[item_choice] ? (inventory_amount ? "Done" : "Edit amount") : (have ? "Relock" : "Unlock");
    // ((Triforce) no "Move" for Up and Down while an amount is changed: they do nothing then)
    ui::Prompt prompts[5];
    int prompts_n = 0;
    if (!inventory_amount) prompts[prompts_n++] = {ui::kUpDown, "Move"};
    prompts[prompts_n++] = {ui::kLeftRight, inventory_amount ? "Amount" : "Move"};
    prompts[prompts_n++] = {ui::kTabs, "Section"};
    if (have >= 0) prompts[prompts_n++] = {ui::kConfirm, edit};
    prompts[prompts_n++] = {ui::kBack, inventory_amount ? "Done" : "Back"};
    float cx0, cx1, hx0, hx1;
    bool narrow;
    columns(c, &cx0, &cx1, &hx0, &hx1, &narrow);
    column(c, cx0, cx1, hh);
    const float bar = ui::prompt_bar_top(c);   // (the strip itself drawn last, over all: below)
    const float left = cx0 + 24 * s, right = cx1 - 24 * s;
    const float gap = 10 * s;
    // the sections, as tabs over the grid (Q and E, or a click)
    // (on one line: their padding, then their text, made smaller as far as needed for the column's width)
    float tpx = 16 * s, tpad = 26 * s, tgap = 8 * s;
    auto tabs_w = [&] {
        float w = 0;
        for (int i = 0; i <= kItemSections; ++i) w += ui::size_of(c.f.strong, tpx, i ? item_sections.s[i - 1].name : "All").x + tpad + tgap;
        return w - tgap;
    };
    if (tabs_w() > right - left) tpad = 16 * s, tgap = 6 * s;
    if (const float w = tabs_w(); w > right - left) {
        const float text = w - (kItemSections + 1) * tpad - kItemSections * tgap;
        tpx = std::max(12 * s, tpx * (right - left - (w - text)) / text);
    }
    float x = left, y = hh + 18 * s;
    for (int i = 0; i <= kItemSections; ++i) {
        const char* label = i ? item_sections.s[i - 1].name : "All";
        const float w = ui::size_of(c.f.strong, tpx, label).x + tpad;
        if (x > left && x + w > right) x = left, y += 36 * s;
        const ImVec2 a(x, y), b(std::min(x + w, right), y + 30 * s);
        const bool active = i == inventory_filter;
        ui::value_plate(c, a, b, active);
        ui::text_mid_centre(c, c.f.strong, tpx, (a.x + b.x) * .5f, y + 15 * s, active ? ui::kValueOnInk : ui::kInk, label);
        hits.add(a.x, a.y, b.x, b.y, menu_hit::kRow, i);
        x += w + tgap;
    }
    float top = y + 44 * s;
    const float pitch = (loaded ? 62 : 46) * s;   // (the title screen: the names alone, no line under them)
    if (!loaded) {   // (the title screen: one note for the page, not "Load a save" on every card)
        ui::diamond(c, ImVec2(left + 5 * s, top - 2 * s), 4 * s, ui::kGoldHi);
        ui::text_mid(c, c.f.body, 16 * s, left + 18 * s, top - 2 * s, ui::kSoft, "Load a save to edit the inventory.");
        top += 30 * s;
    }
    const float strip = 58 * s, detail = bar - 14 * s - strip, grid_bottom = std::max(top, detail - 8 * s);
    // ((Triforce) the card shows a change at once; the game makes it when it runs, as the name pages say of theirs)
    const char* help = have < 0 ? "Load a save to edit this item." : item_kinds[item_choice] ?
        "Choose Edit amount, then use Left and Right to change the quantity. The game takes a change when it runs." :
        "Unlock or relock this item. The game takes a change when it runs.";
    caption(c, hx0, hx1, item_names[item_choice], help, nullptr, nullptr);
    // (as many columns as fit the longest name, at most 5; whole pages of cards, the range and the page under them)
    static float name_w = 0, name_s = 0;
    if (name_s != s) {
        name_s = s, name_w = 0;
        for (int i = 0; i < kItems; ++i) name_w = std::max(name_w, ui::size_of(c.f.strong, 15.5f * s, item_names[i]).x);
    }
    inventory_columns = std::clamp((int)((right - left + gap) / std::max(name_w + 26 * s + gap, 150 * s)), 1, 5);
    inventory_capacity = inventory_columns * std::max(1, (int)((grid_bottom - top + gap) / pitch));
    const int pages_n = std::max(1, (end - begin + inventory_capacity - 1) / inventory_capacity);
    inventory_page = std::clamp(inventory_page, 0, pages_n - 1);
    const int first = begin + inventory_page * inventory_capacity;
    const float width = (right - left + gap) / inventory_columns;
    c.d->PushClipRect(ImVec2(left - 12 * s, top - 12 * s), ImVec2(right + 12 * s, grid_bottom + 12 * s));
    for (int i = first; i < std::min(end, first + inventory_capacity); ++i) {
        const int cell = i - first;
        const ImVec2 a(left + (cell % inventory_columns) * width, top + (cell / inventory_columns) * pitch);
        const ImVec2 b(a.x + width - gap, a.y + pitch - gap);
        const int count = debug_pending::shown(pending, i, item_counts[i].load(std::memory_order_relaxed));
        const bool selected = i == item_choice, owned = count > 0, editing = selected && inventory_amount;
        ui::plate(c, a, b, owned);
        c.d->PushClipRect(a + ImVec2(8 * s, 0), b - ImVec2(8 * s, 0), true);
        ui::text_mid(c, c.f.strong, 15.5f * s, a.x + 13 * s, loaded ? a.y + 17 * s : (a.y + b.y) * 0.5f, count < 0 ? ui::kDim : ui::kInkBright, item_names[i]);
        char amount[40];
        if (count < 0) snprintf(amount, sizeof amount, "%s", loaded ? "Unavailable" : "");
        else if (item_kinds[i]) snprintf(amount, sizeof amount, "Quantity %d", count);
        else snprintf(amount, sizeof amount, "%s", count ? "Unlocked" : "Locked");
        const float ly = a.y + 37 * s;
        if (editing) {   // (the amount being changed: its arrows, gold)
            ui::triangle(c, ImVec2(a.x + 18 * s, ly), 8 * s, 0, ui::kGoldHi);
            ui::text_mid(c, c.f.strong, 15 * s, a.x + 28 * s, ly, ui::kGoldHi, amount);
            ui::triangle(c, ImVec2(a.x + 38 * s + ui::size_of(c.f.strong, 15 * s, amount).x, ly), 8 * s, 1, ui::kGoldHi);
        } else {
            ui::text_mid(c, c.f.body, 15 * s, a.x + 13 * s, ly, owned ? ui::kAccent : kCardOff, amount);
        }
        c.d->PopClipRect();
        if (selected) ui::glow(c, a, b);
        if (a.y < grid_bottom) hits.add(a.x, a.y, b.x, std::min(b.y, grid_bottom), menu_hit::kListLine, i);
    }
    c.d->PopClipRect();
    if (detail >= top) {   // (the page: its arrows, Page n of m and the range; the selected item's action at the right)
        c.d->AddLine(ImVec2(left, detail), ImVec2(right, detail), ui::alpha(ui::kEdge, 0.8f));
        const int pages = (end - begin + inventory_capacity - 1) / inventory_capacity, at = (first - begin) / inventory_capacity;
        const float my = detail + strip * 0.5f + 2 * s, side = 32 * s;
        float px0 = left;
        if (pages > 1) {
            page_arrow(c, ImVec2(px0, my - side * 0.5f), side, 0, at > 0);
            hits.add(px0, my - side * 0.5f, px0 + side, my + side * 0.5f, menu_hit::kButton, 2);
            char page_text[32];
            snprintf(page_text, sizeof page_text, "Page %d of %d", at + 1, pages);
            const float tw = ui::size_of(c.f.strong, 17 * s, page_text).x;
            ui::text_mid(c, c.f.strong, 17 * s, px0 + side + 12 * s, my, ui::kInkBright, page_text);
            const float nx = px0 + side + 24 * s + tw;
            page_arrow(c, ImVec2(nx, my - side * 0.5f), side, 1, at + 1 < pages);
            hits.add(nx, my - side * 0.5f, nx + side, my + side * 0.5f, menu_hit::kButton, 3);
            px0 = nx + side + 18 * s;
        }
        char range[64];
        snprintf(range, sizeof range, "%d\xe2\x80\x93%d of %d", first - begin + 1, std::min(end, first + inventory_capacity) - begin, end - begin);
        ui::text_mid(c, c.f.body, 15 * s, px0, my, ui::kSoft, range);
        if (have >= 0) {
            const float bw = ui::size_of(c.f.strong, 18 * s, "Edit amount").x + 36 * s;
            const ImVec2 a(right - bw, my - 17 * s), b(right, my + 17 * s);
            ui::button(c, a, b, edit, false);
            hits.add(a.x, a.y, b.x, b.y, menu_hit::kButton, 0);
            if (inventory_amount) {
                char amount[32];
                snprintf(amount, sizeof amount, "%d", have);
                const ImVec2 av(a.x - 118 * s, a.y), bv(a.x - 10 * s, b.y);
                choice_cell(c, av, bv, amount, true, true, true);
                hits.add(av.x, av.y, bv.x, bv.y, menu_hit::kButton, 1, av.x, bv.x);
            }
        }
    }
    ui::prompt_bar(c, prompts, prompts_n);   // (the prompts, on top of the help and the cards)
}

void draw_teleport(ui::Ctx& c) {
    const float s = c.s;
    char count[32];
    snprintf(count, sizeof count, "%d of %d", teleport_choice + 1, kTeleports);
    const float hh = title_banner(c, "Teleport", count);
    float cx0, cx1, hx0, hx1;
    bool narrow;
    columns(c, &cx0, &cx1, &hx0, &hx1, &narrow);
    column(c, cx0, cx1, hh);
    const ui::Prompt prompts[] = {{ui::kUpDown, "Move"}, {ui::kLeftRight, "Move"}, {ui::kTabs, "Section"},
        {ui::kConfirm, "Travel"}, {ui::kBack, "Back"}};
    const float bar = ui::prompt_bar_top(c);   // (the strip itself drawn last, over all: below)
    const float left = cx0 + 24 * s, right = cx1 - 24 * s, top = hh + 16 * s;
    const float strip = 58 * s, detail = bar - 14 * s - strip;
    const float bottom = std::max(top, detail - 6 * s), gap = 10 * s, pitch = 48 * s, head = 40 * s;
    const char* const note = teleport_notes[teleport_choice];
    caption(c, hx0, hx1, teleport_names[teleport_choice],
            "Select a destination, then Travel to go to its map entrance. Link keeps his current age.", note, nullptr);   // ((Triforce) no "Enter": the guides name the key, a controller's too)
    // (as many columns as fit the longest name and its tag, at most 4; each section under its heading, the cards scrolled to
    // keep the selected one in view)
    static float card_w = 0, card_s = 0;
    if (card_s != s) {
        card_s = s, card_w = 0;
        for (int i = 0; i < kTeleports; ++i)
            card_w = std::max(card_w, ui::size_of(c.f.body, 17 * s, teleport_names[i]).x +
                                          (teleport_notes[i] ? card_tag(c, 0, 0, teleport_notes[i], false) + 12 * s : 0));
    }
    teleport_columns = std::clamp((int)((right - left + gap) / (card_w + 36 * s + gap)), 1, 4);
    const int cols = teleport_columns;
    const float width = (right - left + gap) / cols;
    // (the y of each section's heading and first row, from the top of the list)
    float section_y[kTeleportSections + 1];
    float total = 0;
    for (int k = 0; k < kTeleportSections; ++k) {
        section_y[k] = total;
        const int n = teleport_section_end(k) - teleport_sections[k].first;
        total += head + (n + cols - 1) / cols * pitch + (k + 1 < kTeleportSections ? 10 * s : 0);
    }
    section_y[kTeleportSections] = total;
    auto card_y = [&](int i) {
        const int k = teleport_section_of(i);
        return section_y[k] + head + (i - teleport_sections[k].first) / cols * pitch;
    };
    const float view = bottom - top;
    teleport_page_rows = std::max(1, (int)(view / pitch) - 1);
    float& at = teleport_scroll_px;
    float& want = teleport_want_px;
    if (teleport_wheel) want -= teleport_wheel * pitch, teleport_wheel = 0;   // (the wheel: the cards move, the choice stays)
    if (total <= view) want = 0;
    else if (!teleport_follow) want = std::clamp(want, 0.0f, total - view);
    else {
        const int k = teleport_section_of(teleport_choice);
        const bool first_row = teleport_choice - teleport_sections[k].first < cols;
        const float y0 = first_row ? section_y[k] : card_y(teleport_choice) - pitch * 0.5f, y1 = card_y(teleport_choice) + pitch * 1.5f;
        if (y0 < want) want = y0;
        if (y1 > want + view) want = y1 - view;
        want = std::clamp(want, 0.0f, total - view);
    }
    at += (want - at) * ease_step;
    if (std::fabs(want - at) < 0.5f) at = want;
    if (total > view) scroll_bar(c, cx0 + 12 * s, top, bottom, (int)total, (int)view, at);
    c.d->PushClipRect(ImVec2(left - 12 * s, top), ImVec2(right + 12 * s, bottom), true);
    for (int k = 0; k < kTeleportSections; ++k) {
        const float hy = top + section_y[k] - at + head * 0.5f;
        if (hy > bottom + head || hy < top - head) continue;
        ui::text_mid(c, c.f.heading, 20 * s, left, hy, ui::kInkBright, teleport_sections[k].name);
        const float tw = ui::size_of(c.f.heading, 20 * s, teleport_sections[k].name).x;
        char n_text[16];
        snprintf(n_text, sizeof n_text, "%d", teleport_section_end(k) - teleport_sections[k].first);
        const float nw = ui::size_of(c.f.body, 15 * s, n_text).x;
        ui::text_mid(c, c.f.body, 15 * s, right - nw, hy, ui::kSoft, n_text);
        c.d->AddLine(ImVec2(left + tw + 14 * s, hy + s), ImVec2(right - nw - 12 * s, hy + s), ui::alpha(ui::kAccent, 0.45f),
                     std::max(1.0f, s));
    }
    for (int i = 0; i < kTeleports; ++i) {
        const int k = teleport_section_of(i), cell = i - teleport_sections[k].first;
        const ImVec2 a(left + cell % cols * width, top + card_y(i) - at);
        const ImVec2 b(a.x + width - gap, a.y + pitch - 8 * s);
        if (b.y < top || a.y > bottom) continue;
        const bool selected = i == teleport_choice;
        ui::plate(c, a, b, selected);
        float name_end = b.x - 10 * s;
        if (teleport_notes[i]) name_end -= card_tag(c, b.x - 8 * s, (a.y + b.y) * 0.5f, teleport_notes[i]) + 8 * s;
        c.d->PushClipRect(a, ImVec2(name_end, b.y), true);
        ui::text_mid(c, c.f.body, 17 * s, a.x + 16 * s, (a.y + b.y) * 0.5f, ui::kInk, teleport_names[i]);
        c.d->PopClipRect();
        if (selected) ui::glow(c, a, b);
        hits.add(a.x, std::max(a.y, top), b.x, std::min(b.y, bottom), menu_hit::kListLine, i);
    }
    c.d->PopClipRect();
    if (detail >= top) {   // (the selection, its section and note, and Travel: what Enter does)
        c.d->AddLine(ImVec2(left, detail), ImVec2(right, detail), ui::alpha(ui::kEdge, 0.8f));
        const float my = detail + strip * 0.5f + 2 * s;
        const float bw = ui::size_of(c.f.strong, 20 * s, "Travel").x + 56 * s;
        const ImVec2 a(right - bw, my - 18 * s), b(right, my + 18 * s);
        c.d->PushClipRect(ImVec2(left, detail), ImVec2(a.x - 12 * s, detail + strip), true);
        ui::text_mid(c, c.f.strong, 18 * s, left, my - 10 * s, ui::kInkBright, teleport_names[teleport_choice]);
        char where[64];
        snprintf(where, sizeof where, note ? "%s \xc2\xb7 %s" : "%s", teleport_sections[teleport_section_of(teleport_choice)].name, note);
        ui::text_mid(c, c.f.body, 15 * s, left, my + 12 * s, ui::kAccent, where);
        c.d->PopClipRect();
        ui::button(c, a, b, "Travel", false);
        hits.add(a.x, a.y, b.x, b.y, menu_hit::kButton, 0);
    }
    ui::prompt_bar(c, prompts, 5);   // (the prompts, on top of the help and the cards)
}

/// (Triforce) A name page (Game flags, Cutscenes, Scan amiibo): the group shown as a choice across the top (L and R, or a
/// click on its arrows), the group's names as cards in whole pages, and under them the page's arrows, the range and the
/// chosen name's action; the caption has the chosen name whole, what it is and what the game's side last said
void draw_names(ui::Ctx& c) {
    const float s = c.s;
    dn::Page& p = names_page[names_kind];
    const bool flags = names_kind == kNamesFlags, events = names_kind == kNamesEvents;
    static const char* const titles[kNamesKinds] = {"Game flags", "Cutscenes", "Scan amiibo"};   // ((Triforce) as their rows say)
    char count_text[48];
    snprintf(count_text, sizeof count_text, flags ? "%d flags" : events ? "%d events" : "%d files", (int)p.list.names.size());
    const float hh = title_banner(c, titles[names_kind], p.empty() ? nullptr : count_text);
    float cx0, cx1, hx0, hx1;
    bool narrow;
    columns(c, &cx0, &cx1, &hx0, &hx1, &narrow);
    column(c, cx0, cx1, hh);
    const float bar = ui::prompt_bar_top(c);   // (the strip itself drawn last, over all: below)
    const float left = cx0 + 24 * s, right = cx1 - 24 * s, gap = 10 * s;
    const float y = hh + 18 * s, top = y + 50 * s, pitch = (flags ? 62 : 46) * s;   // (a flag's card: its state under its name)
    const float strip = 58 * s, detail = bar - 14 * s - strip, grid_bottom = std::max(top, detail - 8 * s);
    // (as many columns as fit, at most 3: the names are long; whole pages of cards, never more than a W line's flags)
    p.columns = std::clamp((int)((right - left + gap) / (340 * s + gap)), 1, 3);
    p.capacity = std::min(p.columns * std::max(1, (int)((grid_bottom - top + gap) / pitch)), (int)OOT_WATCH_MAX);
    p.shown = std::clamp(p.shown, 0, p.pages() - 1);
    // (what the game's side last said, while the chosen name is the one it said it of: under another name it would read as
    // that name's. A flag's or an event's line, or what became of the figure whose file was sent)
    std::string status;
    std::string states;   // (the shown flags' states, a character each from p.first())
    char state = '?';     // (the chosen flag's)
    const int figure = amiibo_said.load(std::memory_order_relaxed);
    {
        std::lock_guard lk(names_mu);
        if (flags) {
            for (int i = p.first(); i < p.last(); ++i) states += names_states.of(p.list.hashes[i]);
            if (!p.empty()) state = names_states.of(p.list.hashes[p.choice]);
        }
        if (!p.empty() && names_kind == names_status_kind && names_status_of == p.list.names[p.choice]) status = names_status;
    }
    if (!p.empty() && names_kind == kNamesAmiibo && amiibo_chosen == p.list.names[p.choice])
        if (const char* text = amiibo_text(figure, false)) status = text;
    if (p.empty()) {   // (no names yet, or none at all: one note for the page)
        const int scan = names_scan.load(std::memory_order_acquire);
        const char* note = names_kind == kNamesAmiibo ? "No dumps yet. Put .bin dumps of your own figures into the folder \"amiibo\" in the game folder."
                           : scan == 3 ? "The game's files were not found." : scan == 2 ? "Your game files name none." : "Reading the names from your game files...";
        ui::diamond(c, ImVec2(left + 5 * s, hh + 34 * s), 4 * s, ui::kGoldHi);
        ui::para(c, c.f.body, 16 * s, ImVec2(left + 18 * s, hh + 24 * s), ui::kSoft, note, right - left - 18 * s);
        caption(c, hx0, hx1, titles[names_kind], names_help(names_kind), nullptr, nullptr);
        const ui::Prompt back[] = {{ui::kBack, "Back"}};
        ui::prompt_bar(c, back, 1);
        return;
    }
    const std::string& chosen = p.list.names[p.choice];
    const bool figure_waits = names_kind == kNamesAmiibo && amiibo_chosen == chosen && (figure == HOS_AMIIBO_WAITING || figure == HOS_AMIIBO_ON_READER);
    const char* action = flags ? (names_edit ? "Done" : "Change") : events ? "Start" : figure_waits ? "Take away" : "Scan";
    const int groups = (int)p.list.groups.size();
    // (the guides say what the keys do here: Up and Down nothing while a flag's On and Off are chosen, L and R nothing with one
    // group)
    ui::Prompt prompts[5];
    int prompts_n = 0;
    if (!names_edit) prompts[prompts_n++] = {ui::kUpDown, "Move"};
    prompts[prompts_n++] = {ui::kLeftRight, names_edit ? "On / Off" : "Move"};
    if (groups > 1) prompts[prompts_n++] = {ui::kTabs, "Group"};
    prompts[prompts_n++] = {ui::kConfirm, action};
    prompts[prompts_n++] = {ui::kBack, names_edit ? "Done" : "Back"};
    // the group shown: a choice with its arrows, the group's place among them at its right
    const dn::Group& group = p.list.groups[p.group];
    {
        char label[160], place[32];
        snprintf(label, sizeof label, "%s (%d)", group.title.c_str(), group.count);
        const ImVec2 a(left, y), b(std::min(right, left + 560 * s), y + 36 * s);
        choice_cell(c, a, b, label, false, groups > 1, groups > 1);
        if (groups > 1) {
            hits.add(a.x, a.y, b.x, b.y, menu_hit::kButton, 4, a.x, b.x);
            snprintf(place, sizeof place, "Group %d of %d", p.group + 1, groups);
            if (b.x + 16 * s + ui::size_of(c.f.body, 15 * s, place).x <= right) ui::text_mid(c, c.f.body, 15 * s, b.x + 16 * s, y + 18 * s, ui::kSoft, place);
        }
    }
    {   // the caption: what the page is, the chosen name whole (a long one takes lines), what it is now, and the last answer
        std::string about = chosen;
        if (flags) about += state == '1' ? "\nSet in this save." : state == '0' ? "\nNot set in this save." : state == '-' ? "\nThis save's data has no such flag." :
                            "\nNot known now: no save is loaded, or the game is paused.";
        // (the last answer: the line at the caption's foot when it fits that one line, else with the text above it, which takes
        // lines: a line cut off at the plate's edge said half of "the file is not a figure's dump")
        const float line_w = hx1 - hx0 - 2 * ui::lu(c, 72) - 20 * s;
        const bool one_line = !status.empty() && ui::size_of(c.f.strong, kHintPt * s, status.c_str()).x <= line_w;
        if (!status.empty() && !one_line) about += "\n" + status;
        caption(c, hx0, hx1, titles[names_kind], names_help(names_kind), about.c_str(), one_line ? status.c_str() : nullptr);
    }
    const int first = p.first(), last = p.last();
    const float width = (right - left + gap) / p.columns;
    c.d->PushClipRect(ImVec2(left - 12 * s, top - 12 * s), ImVec2(right + 12 * s, grid_bottom + 12 * s));
    for (int i = first; i < last; ++i) {
        const int cell = i - first;
        const ImVec2 a(left + (cell % p.columns) * width, top + (cell / p.columns) * pitch);
        const ImVec2 b(a.x + width - gap, a.y + pitch - gap);
        const char st = flags && cell < (int)states.size() ? states[cell] : '?';
        const bool selected = i == p.choice;
        const bool sent = names_kind == kNamesAmiibo && amiibo_chosen == p.list.names[i];   // (the file the figure is of)
        // (the figure's card says where the figure is, whichever card is chosen: a tag at its right end, as the Teleport's, and
        // lit while it has one. (Triforce) Not lit for a file the game refused or could not read: that read as the one in use)
        const char* tag = !sent ? nullptr : figure == HOS_AMIIBO_WAITING ? "waiting" : figure == HOS_AMIIBO_ON_READER ? "on the reader" :
                          figure == HOS_AMIIBO_READ ? "read" : nullptr;
        ui::plate(c, a, b, flags ? st == '1' : tag != nullptr);
        float name_end = b.x - 13 * s;
        if (tag) name_end -= card_tag(c, b.x - 8 * s, (a.y + b.y) * 0.5f, tag) + 8 * s;
        c.d->PushClipRect(a + ImVec2(8 * s, 0), ImVec2(name_end, b.y), true);
        // (the name without what its group is named for, and cut in its middle when it is still too long: the names of a group
        // begin alike and differ at their ends, which a cut at the end left out; the caption has the chosen one whole)
        const float room = name_end - (a.x + 13 * s);
        const std::string name = dn::elide_middle(dn::card_label(p.list, p.group, i),
                                                  [&](const std::string& t) { return ui::size_of(c.f.strong, 15.5f * s, t.c_str()).x <= room; });
        ui::text_mid(c, c.f.strong, 15.5f * s, a.x + 13 * s, flags ? a.y + 17 * s : (a.y + b.y) * 0.5f, ui::kInkBright, name.c_str());
        if (flags)   // (the state is what the page is for: legible when it is not Set too; kDim on a plate could not be read)
            ui::text_mid(c, c.f.body, 15 * s, a.x + 13 * s, a.y + 37 * s, st == '1' ? ui::kAccent : st == '0' ? ui::kSoft : kCardOff,
                         st == '1' ? "Set" : st == '0' ? "Not set" : st == '-' ? "Not in this save" : "Not known");
        c.d->PopClipRect();
        if (selected) ui::glow(c, a, b);
        if (a.y < grid_bottom) hits.add(a.x, a.y, b.x, std::min(b.y, grid_bottom), menu_hit::kListLine, i);
    }
    c.d->PopClipRect();
    if (detail >= top) {   // (the page: its arrows, Page n of m and the range; the chosen name's action at the right)
        c.d->AddLine(ImVec2(left, detail), ImVec2(right, detail), ui::alpha(ui::kEdge, 0.8f));
        const int pages = p.pages(), at = (first - p.begin()) / std::max(1, p.capacity);
        const float my = detail + strip * 0.5f + 2 * s, side = 32 * s;
        float px0 = left;
        if (pages > 1) {
            page_arrow(c, ImVec2(px0, my - side * 0.5f), side, 0, at > 0);
            hits.add(px0, my - side * 0.5f, px0 + side, my + side * 0.5f, menu_hit::kButton, 2);
            char page_text[32];
            snprintf(page_text, sizeof page_text, "Page %d of %d", at + 1, pages);
            const float tw = ui::size_of(c.f.strong, 17 * s, page_text).x;
            ui::text_mid(c, c.f.strong, 17 * s, px0 + side + 12 * s, my, ui::kInkBright, page_text);
            const float nx = px0 + side + 24 * s + tw;
            page_arrow(c, ImVec2(nx, my - side * 0.5f), side, 1, at + 1 < pages);
            hits.add(nx, my - side * 0.5f, nx + side, my + side * 0.5f, menu_hit::kButton, 3);
            px0 = nx + side + 18 * s;
        }
        char range[64];
        snprintf(range, sizeof range, "%d\xe2\x80\x93%d of %d", first - p.begin() + 1, last - p.begin(), p.end() - p.begin());   // ((Triforce) as the Inventory's)
        ui::text_mid(c, c.f.body, 15 * s, px0, my, ui::kSoft, range);
        const float bw = ui::size_of(c.f.strong, 18 * s, "Take away").x + 36 * s;
        const ImVec2 a(right - bw, my - 17 * s), b(right, my + 17 * s);
        ui::button(c, a, b, action, false);
        hits.add(a.x, a.y, b.x, b.y, menu_hit::kButton, 0);
        if (names_edit) {   // (a flag's two halves: On at the left, Off at the right, the one it is lit when that is known)
            const ImVec2 av(a.x - 190 * s, a.y), bv(a.x - 10 * s, b.y);
            const float xm = av.x + (bv.x - av.x) * 0.632f;   // (where halves_cell's two halves meet)
            if (state == '1' || state == '0') {
                halves_cell(c, av, bv, "On", "Off", state == '0', true, false);
            } else {   // (not known which it is: neither half lit, and neither dim, for both can be had; dim halves read as "cannot".
                       // (Triforce) The save has no such flag ('-'): neither lit, both dim, for neither can be had)
                const float mid = (av.y + bv.y) * 0.5f, px = std::min(ui::lu(c, 54 * ui::kType), (bv.y - av.y) * 0.62f);
                const ImU32 ink = state == '-' ? ui::kDim : ui::kInk;
                ui::value_plate(c, av, bv);
                ui::text_mid_centre(c, c.f.body, px, (av.x + xm) * 0.5f, mid - ui::lu(c, 2), ink, "On");
                ui::text_mid_centre(c, c.f.body, px, (xm + bv.x) * 0.5f, mid - ui::lu(c, 2), ink, "Off");
                c.d->AddLine(ImVec2(xm, av.y + 8 * s), ImVec2(xm, bv.y - 8 * s), ui::alpha(ui::kEdge, 0.9f), std::max(1.0f, s));
                ui::cursor(c, av - ImVec2(ui::lu(c, 2.5f), ui::lu(c, 3)), bv + ImVec2(ui::lu(c, 2.5f), ui::lu(c, 3)));
            }
            hits.add(av.x, av.y, xm, bv.y, menu_hit::kButton, 1, 0, 0, 0, 0, -1);
            hits.add(xm, av.y, bv.x, bv.y, menu_hit::kButton, 1, 0, 0, 0, 0, 1);
        }
    }
    ui::prompt_bar(c, prompts, prompts_n);   // (the prompts, on top of the help and the cards)
}

// ---- (Triforce) the Mods page: GameBanana's mods for this game (vk_mods.h) ----

std::string mods_size(uint64_t b) {
    char t[32];
    if (b >= (1ull << 30)) snprintf(t, sizeof t, "%.1f GB", b / (double)(1ull << 30));
    else if (b >= (1ull << 20)) snprintf(t, sizeof t, "%.1f MB", b / (double)(1ull << 20));
    else snprintf(t, sizeof t, "%.0f KB", b / 1024.0);
    return t;
}
std::string mods_count(int n) {   // ("3,638")
    std::string t = std::to_string(n);
    for (int i = (int)t.size() - 3; i > 0; i -= 3) t.insert((size_t)i, ",");
    return t;
}
/// (Triforce, 11-17) A texture let go: the backend frees it while it is still registered (ImGui hands it the texture list of
/// the registered ones only), so it is unregistered and deleted once it is Destroyed, by textures_reap. Unregistered at once,
/// its image and descriptor set were never freed: the Mods screen's pictures used up the overlays' pool that way
std::vector<ImTextureData*> textures_retiring;
void texture_retire(ImTextureData* t) {
    if (!t) return;
    // (WantDestroyNextFrame too: ImGui's SetStatus(Destroyed) makes a texture that still has its pixels WantCreate again
    // unless it is set, and the backend's destroy is then a re-creation, every frame)
    t->WantDestroyNextFrame = true;
    t->SetStatus(ImTextureStatus_WantDestroy);
    textures_retiring.push_back(t);
    static int let_go = 0;
    if (++let_go % 50 == 1) logf("menu: %d pictures let go so far (far out of view, or resized)", let_go);
}
void textures_reap() {
    for (size_t i = 0; i < textures_retiring.size();) {
        ImTextureData* t = textures_retiring[i];
        if (t->Status != ImTextureStatus_Destroyed) { i++; continue; }
        ImGui::UnregisterUserTexture(t);
        IM_DELETE(t);
        textures_retiring[i] = textures_retiring.back(), textures_retiring.pop_back();
    }
}
/// A mod's picture over a..b, cut to the box's shape (its middle kept); its texture made from the pixels once they came
void mods_picture(ui::Ctx& c, mods::Mod& m, ImVec2 a, ImVec2 b, int* made) {
    if (!m.texture && m.thumb_state == 2 && !m.thumb_rgba.empty() && *made < 3) {   // (a few a frame: no hitch while scrolling)
        ImTextureData* t = IM_NEW(ImTextureData)();
        t->Create(ImTextureFormat_RGBA32, m.thumb_w, m.thumb_h);
        memcpy(t->GetPixels(), m.thumb_rgba.data(), m.thumb_rgba.size());
        t->UseColors = true;
        ImGui::RegisterUserTexture(t);
        m.texture = t;
        m.thumb_rgba.clear(), m.thumb_rgba.shrink_to_fit();
        (*made)++;
    }
    c.d->AddRectFilled(a, b, ui::rgba(18, 13, 8, 255));
    if (ImTextureData* t = (ImTextureData*)m.texture) {
        const float bw = b.x - a.x, bh = b.y - a.y, iw = (float)m.thumb_w, ih = (float)m.thumb_h;
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (iw / ih > bw / bh) { const float k = (bh / bw) * iw / ih; uv0.x = (1 - 1 / k) * 0.5f, uv1.x = 1 - uv0.x; }   // (wider: its sides cut)
        else { const float k = (bw / bh) * ih / iw; uv0.y = (1 - 1 / k) * 0.5f, uv1.y = 1 - uv0.y; }                     // (taller: top and foot cut)
        c.d->AddImage(t->GetTexRef(), a, b, uv0, uv1);
    } else {
        const char* what = m.thumb_state == 3 ? "No picture" : "...";
        ui::text_mid_centre(c, c.f.body, 13 * c.s, (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, ui::kDim, what);
    }
    c.d->AddRect(a, b, ui::alpha(ui::kEdge, 0.9f), 0, std::max(1.0f, c.s));
}

/// A dialog over the page: the game's caption plate in the middle, its title, its text, and up to two buttons (the upper one
/// first); the progress bar between them when frac >= 0
void mods_dialog(ui::Ctx& c, const char* title, const std::string& text, const char* const* labels, int n, float frac = -1,
                 const char* bar_left = nullptr, const char* bar_right = nullptr) {
    const float s = c.s;
    c.d->AddRectFilled(ImVec2(0, 0), ImVec2(c.w, c.h), ui::rgba(8, 5, 2, 120));
    const float w = ui::lu(c, 1500), pad = ui::lu(c, 80), px = kHelpPt * s;
    const float th = ui::para(c, c.f.body, px, ImVec2(), 0, text.c_str(), w - 2 * pad, false);
    const float bar_h = frac >= 0 ? 74 * s : 0, btn_h = ui::lu(c, 144), btns = n ? n * (btn_h + ui::lu(c, 44)) : 0;
    const float tall = pad + 34 * s + th + 18 * s + bar_h + btns + pad * 0.6f;
    const ImVec2 a(c.w * 0.5f - w * 0.5f, std::max(banner_h(c) + 20 * s, c.h * 0.5f - tall * 0.5f)), b(a.x + w, a.y + tall);
    ui::box(c, a, b);
    float y = a.y + pad * 0.7f;
    ui::text_mid_centre(c, c.f.strong, 21.6f * s, c.w * 0.5f, y + 10 * s, ui::kInk, title);
    y += 34 * s;
    {   // (each line centred, as the game's dialogs set their text)
        const char* t = text.c_str();
        const char* end = t + text.size();
        while (t < end) {
            const char* nl = (const char*)memchr(t, '\n', (size_t)(end - t));
            const char* line_end = nl ? nl : end;
            const char* cut = c.f.body->CalcWordWrapPosition(px, t, line_end, w - 2 * pad);
            if (cut == t) cut = t + 1;
            const float lw = c.f.body->CalcTextSizeA(px, FLT_MAX, 0, t, cut).x;
            c.d->AddText(c.f.body, px, ImVec2(c.w * 0.5f - lw * 0.5f, y), ui::kSoft, t, cut);
            y += px * 1.3f;
            t = cut;
            while (t < end && (*t == ' ' || *t == '\n')) t++;
        }
    }
    y += 18 * s;
    if (frac >= 0) {   // (the bar: the game's value channel, filled in gold as the HUD's magic, the numbers over it)
        const float bx0 = a.x + pad, bx1 = b.x - pad, by = y + 30 * s;
        if (bar_left) ui::text_mid(c, c.f.body, 15 * s, bx0, y + 8 * s, ui::kSoft, bar_left);
        if (bar_right) ui::text_mid_right(c, c.f.body, 15 * s, bx1, y + 8 * s, ui::kSoft, bar_right);
        ui::value_plate(c, ImVec2(bx0, by), ImVec2(bx1, by + 20 * s));
        const float fx = bx0 + 3 * s + (bx1 - bx0 - 6 * s) * std::clamp(frac, 0.0f, 1.0f);
        if (fx > bx0 + 3 * s) {
            c.d->AddRectFilledMultiColor(ImVec2(bx0 + 3 * s, by + 3 * s), ImVec2(fx, by + 17 * s), ui::rgba(170, 236, 120), ui::rgba(170, 236, 120),
                                         ui::rgba(52, 160, 52), ui::rgba(52, 160, 52));
            c.d->AddRectFilled(ImVec2(bx0 + 3 * s, by + 4 * s), ImVec2(fx, by + 7 * s), ui::rgba(255, 255, 255, 80));
        }
        y += bar_h;
    }
    const float bw = ui::lu(c, 336);
    for (int i = 0; i < n; i++) {
        const float my = y + btn_h * 0.5f + i * (btn_h + ui::lu(c, 44));
        const ImVec2 ba(c.w * 0.5f - bw, my - btn_h * 0.5f), bb(c.w * 0.5f + bw, my + btn_h * 0.5f);
        ui::button(c, ba, bb, labels[i], i == mods_btn);
        hits.add(ba.x, ba.y, bb.x, bb.y, menu_hit::kButton, i);
    }
}

// (the Salesman: his newest picture from his thread (vk_salesman.h), as a texture updated in place)
ImTextureData* salesman_tex = nullptr;
uint64_t salesman_seq = 0;
int salesman_w = 0, salesman_h = 0;   // (the size his corner has on screen, pixels: what his thread draws for)
bool salesman_said_hello = false;     // (a smile the first time the page shows each run)
int64_t salesman_line_at = 0;
int salesman_line = 0;
const char* const salesman_lines[] = {
    "Oh! A customer with taste... Every one of these has a story.",
    "Take your time. I have all the time in the world.",
    "Each of these was made by someone's own hands. Mostly.",
    "That one... yes. That one would suit you.",
    "A new face for an old friend. How delightful.",
};
void salesman_picture(ui::Ctx& c, ImVec2 a, ImVec2 b) {
    salesman_w = (int)(b.x - a.x), salesman_h = (int)(b.y - a.y);
    static std::vector<unsigned char> buf;
    int w = 0, h = 0;
    if (salesman::take(buf, &w, &h, &salesman_seq)) {
        if (salesman_tex && (salesman_tex->Width != w || salesman_tex->Height != h)) {   // (a new size: a new texture)
            texture_retire(salesman_tex);
            salesman_tex = nullptr;
        }
        if (!salesman_tex) {
            salesman_tex = IM_NEW(ImTextureData)();
            salesman_tex->Create(ImTextureFormat_RGBA32, w, h);
            salesman_tex->UseColors = true;
            memcpy(salesman_tex->GetPixels(), buf.data(), buf.size());
            ImGui::RegisterUserTexture(salesman_tex);
        } else if (salesman_tex->Status == ImTextureStatus_OK) {   // (the last copy done: this one in its place)
            memcpy(salesman_tex->GetPixels(), buf.data(), buf.size());
            const ImTextureRect r{0, 0, (unsigned short)w, (unsigned short)h};
            salesman_tex->Updates.resize(0);
            salesman_tex->Updates.push_back(r);
            salesman_tex->UpdateRect = r;
            salesman_tex->SetStatus(ImTextureStatus_WantUpdates);
        }
    }
    if (salesman_tex && salesman_tex->Width > 0) {   // (his picture's own shape, at the foot of his corner)
        const float k = std::min((b.x - a.x) / salesman_tex->Width, (b.y - a.y) / salesman_tex->Height);
        const float pw = salesman_tex->Width * k, ph = salesman_tex->Height * k;
        const ImVec2 pa((a.x + b.x - pw) * 0.5f, b.y - ph);
        c.d->AddImage(salesman_tex->GetTexRef(), pa, pa + ImVec2(pw, ph));
    }
}
/// What he says, in a box at the foot of his corner: the game's caption plate, his name over it
void salesman_says(ui::Ctx& c, float x0, float x1, float y1, const char* line) {
    const float s = c.s, pad = 12 * s, px = 15 * s, w = x1 - x0;
    const float th = ui::para(c, c.f.body, px, ImVec2(), 0, line, w - 2 * pad, false);
    const ImVec2 a(x0, y1 - th - 2 * pad - 6 * s), b(x1, y1);
    ui::box(c, a, b);
    ui::text_mid(c, c.f.strong, 13 * s, a.x + pad, a.y - 2 * s, ui::kAccent, "Happy Mask Salesman");
    ui::para(c, c.f.body, px, ImVec2(a.x + pad, a.y + pad + 3 * s), ui::kInk, line, w - 2 * pad);
}

/// ((Triforce) 11-17) The search's text in a..b at px, its end kept in view when it is longer; a caret after it while sel
void mods_query_text(ui::Ctx& c, ImVec2 a, ImVec2 b, float px, bool sel, const char* empty_hint) {
    const float my = (a.y + b.y) * 0.5f, room = b.x - a.x;
    c.d->PushClipRect(a, b, true);
    if (mods_query.empty()) {
        ui::text_mid(c, c.f.body, px * 0.92f, a.x, my, ui::alpha(ui::kSoft, 0.55f), empty_hint);
        if (sel && (prof_now() / 500000000LL) % 2 == 0)
            c.d->AddLine(ImVec2(a.x, my - px * 0.55f), ImVec2(a.x, my + px * 0.55f), ui::kInk, std::max(1.5f, 1.6f * c.s));
    } else {
        const float w = ui::size_of(c.f.body, px, mods_query.c_str()).x;
        const float x = w + 6 * c.s > room ? b.x - w - 6 * c.s : a.x;
        ui::text_mid(c, c.f.body, px, x, my, ui::kInkBright, mods_query.c_str());
        if (sel && (prof_now() / 500000000LL) % 2 == 0)
            c.d->AddLine(ImVec2(x + w + 2 * c.s, my - px * 0.55f), ImVec2(x + w + 2 * c.s, my + px * 0.55f), ui::kInk, std::max(1.5f, 1.6f * c.s));
    }
    c.d->PopClipRect();
}

/// ((Triforce) 11-17) The search bar over the cards: a plate as the cards have, a magnifying glass, the text typed or what to
/// do, a caret while it is chosen; at its right what the search found
void mods_search_bar(ui::Ctx& c, ImVec2 a, ImVec2 b, const std::string& searched, mods::Feed feed, int n, bool more) {
    const float s = c.s, my = (a.y + b.y) * 0.5f;
    const bool sel = mods_choice < 0 || mods_kbd;
    ui::plate(c, a, b, sel);
    const ImVec2 g(a.x + 25 * s, my - 2.5f * s);   // (the glass)
    c.d->AddCircle(g, 8.5f * s, ui::kSoft, 24, 2.2f * s);
    c.d->AddLine(g + ImVec2(6 * s, 6 * s), g + ImVec2(12.5f * s, 12.5f * s), ui::kSoft, 3 * s);
    std::string found;
    if (!searched.empty()) {
        if (feed == mods::Feed::Failed) found = "Not reached";
        else if (feed == mods::Feed::Loading && !n) found = "Searching...";
        else found = (n == 1 ? std::string("1 found") : std::to_string(n) + (more ? "+ found" : " found"));
    }
    const float found_w = found.empty() ? 0 : ui::size_of(c.f.body, 15 * s, found.c_str()).x + 16 * s;
    if (!found.empty()) ui::text_mid_right(c, c.f.body, 15 * s, b.x - 14 * s, my, ui::kAccent, found.c_str());
    mods_query_text(c, ImVec2(a.x + 46 * s, a.y), ImVec2(b.x - 14 * s - found_w, b.y), 18 * s, sel && !mods_kbd,
                    sel ? "Type to search, or press Confirm for a keyboard" : "Search mods");
    if (sel && !mods_kbd) ui::glow(c, a, b);
    hits.add(a.x, a.y, b.x, b.y, menu_hit::kSearch, 0);
}

/// ((Triforce) 11-17) The keyboard for a controller: a box over the page as the game's dialogs have, the text at its top, four
/// rows of ten keys (the letters shown capital, typed small) and a row of four wide keys; the chosen one lit as the game's
/// chosen value is, the cursor round it. A click types a key too (kKey)
void mods_keyboard(ui::Ctx& c) {
    const float s = c.s, kw = 62 * s, kh = 50 * s, gap = 7 * s, pad = 24 * s, field_h = 52 * s;
    const float w = 10 * kw + 9 * gap + 2 * pad, h = pad + field_h + 16 * s + 5 * kh + 4 * gap + pad;
    c.d->AddRectFilled(ImVec2(0, 0), ImVec2(c.w, c.h), ui::rgba(8, 5, 2, 120));   // (the page behind it darker)
    const float x0 = std::floor((c.w - w) * 0.5f), y0 = std::max(banner_h(c) + 8 * s, ui::prompt_bar_top(c) - h - 16 * s);
    ui::box(c, ImVec2(x0, y0), ImVec2(x0 + w, y0 + h));
    mods_kbd_box[0] = x0, mods_kbd_box[1] = y0, mods_kbd_box[2] = x0 + w, mods_kbd_box[3] = y0 + h;
    const ImVec2 fa(x0 + pad, y0 + pad), fb(x0 + w - pad, y0 + pad + field_h);
    ui::value_plate(c, fa, fb, false);
    mods_query_text(c, ImVec2(fa.x + 14 * s, fa.y), ImVec2(fb.x - 14 * s, fb.y), 20 * s, true, "Type a word to search for");
    const float ky = fb.y + 16 * s;
    for (int row = 0; row < 5; row++) {
        const int keys = row < 4 ? 10 : 4;
        for (int k = 0; k < keys; k++) {
            const int col0 = row < 4 ? k : kKbdWideAt[k], col1 = row < 4 ? k + 1 : kKbdWideAt[k + 1];
            const ImVec2 ka(x0 + pad + col0 * (kw + gap), ky + row * (kh + gap));
            const ImVec2 kb(x0 + pad + col1 * (kw + gap) - gap, ka.y + kh);
            const bool on = row == mods_kbd_row && (row < 4 ? k == mods_kbd_col : k == kbd_wide(mods_kbd_col));
            ui::value_plate(c, ka, kb, on);
            char label[2] = {row < 4 ? (char)toupper((unsigned char)kKbdRows[row][k]) : 0, 0};
            const char* t = row < 4 ? label : kKbdWide[k];
            const ImU32 ink = on ? ui::kValueOnInk : row == 4 && k == 3 ? ui::kGoldHi : ui::kInk;
            ui::text_mid_centre(c, row < 4 ? c.f.strong : c.f.body, (row < 4 ? 21 : 17) * s, (ka.x + kb.x) * 0.5f, (ka.y + kb.y) * 0.5f, ink, t);
            if (on) ui::glow(c, ka, kb);
            hits.add(ka.x, ka.y, kb.x, kb.y, menu_hit::kKey, row * 10 + col0);
        }
    }
}

void draw_mods(ui::Ctx& c) {
    const float s = c.s, hh = banner_h(c);
    ui::banner(c, hh);
    title_banner(c, "Mods", nullptr);   // (its own screen: opened from the game's System list or the title's, not a tab)
    float cx0, cx1, hx0, hx1;
    bool narrow;
    columns(c, &cx0, &cx1, &hx0, &hx1, &narrow);
    const float bar = ui::prompt_bar_top(c);
    // (the Salesman's corner: the right of the cards' column, by the details; the cards narrower while he is there)
    if (salesman::state() == salesman::State::Idle && host && host->romfs_dir) salesman::start(host->romfs_dir());
    const bool salesman_on = salesman::state() != salesman::State::Failed && mods::available();
    // ((Triforce) 11-17, layout A: the list at the left with the category tabs over it, the details at the right, the
    // Salesman small at the list's foot with his line beside him)
    const float tabs_h = mods::available() && mods::accepted() ? 44 * s : 0;
    const float sales_h = salesman_on && mods::accepted() ? 214 * s : 0;
    const float left = cx0 + 34 * s, right = cx1 - 34 * s, top = hh + 18 * s + tabs_h, bottom = bar - 10 * s - sales_h;
    const float search_h = 50 * s, list_top = top + search_h + 12 * s;   // ((Triforce) the search bar over the cards)
    if (tabs_h > 0) {   // (the categories: a strip of tabs over the list, the chosen one light on dark as the game's own tabs)
        const float ty = hh + 14 * s, th = 32 * s, gap = 8 * s, px = 15 * s;
        float w = 0;
        for (int k = 0; k < kModsCats; k++) w += ui::size_of(c.f.strong, px, kModsCatNames[k]).x + 28 * s + (k ? gap : 0);
        float x = (left + right) * 0.5f - w * 0.5f;
        for (int k = 0; k < kModsCats; k++) {
            const float tw = ui::size_of(c.f.strong, px, kModsCatNames[k]).x + 28 * s;
            const bool on = k == mods_cat;
            c.d->AddRectFilled(ImVec2(x, ty), ImVec2(x + tw, ty + th), on ? ui::rgba(239, 228, 204, 235) : ui::rgba(40, 30, 20, 170), 6 * s);
            ui::text_mid_centre(c, c.f.strong, px, x + tw * 0.5f, ty + th * 0.5f, on ? ui::rgba(58, 43, 27) : ui::kSoft, kModsCatNames[k]);
            hits.add(x, ty, x + tw, ty + th, menu_hit::kTab, 100 + k);
            x += tw + gap;
        }
        ui::text_mid_centre(c, c.f.strong, 14 * s, (left + right) * 0.5f - w * 0.5f - 22 * s, ty + th * 0.5f, ui::kDim, "L");
        ui::text_mid_centre(c, c.f.strong, 14 * s, x + 14 * s, ty + th * 0.5f, ui::kDim, "R");
    }
    if (!mods::available()) {
        caption(c, hx0, hx1, "Mods", "Browsing and installing mods from inside the game is not available on this system yet. Put a mod's "
                "folder (the one holding romfs) into the mods folder beside the game.", nullptr, nullptr);
        const ui::Prompt p[] = {{ui::kBack, "Back"}};
        ui::prompt_bar(c, p, 1);
        return;
    }
    if (!mods::accepted()) {   // (the warning, once: Install at your own risk)
        static const char* const labels[] = {"I understand", "Not now"};
        mods_dialog(c, "Mods from GameBanana",
                    "These mods are made by players and shared on GameBanana. Triforce does not check them: install them at your own "
                    "risk.\nIf one breaks the game, turn it off or remove it in Triforce Setup's Mods page, or start with Play "
                    "without mods there.", labels, 2);
        const ui::Prompt p[] = {{ui::kUpDown, "Move"}, {ui::kConfirm, "Choose"}, {ui::kBack, "Back"}};
        ui::prompt_bar(c, p, 3);
        return;
    }
    std::string err;
    const mods::Feed feed = mods::feed_state(&err);
    const mods::InstallState inst = mods::install_state();
    const bool more = mods::more();
    const std::string searched = mods::search_text();   // ((Triforce) before the lock: it takes it)
    mods::lock();
    std::vector<mods::Mod>& list = mods::list();
    const int n_all = (int)list.size();
    if (n_all) mods_choice = std::clamp(mods_choice, -1, n_all - 1);   // ((Triforce) -1: the search bar)
    std::vector<int> vis;   // ((Triforce) the cards of the category chosen, by their place in the list)
    for (int i = 0; i < n_all; i++)
        if (mods_in_cat(list[i], mods_cat)) vis.push_back(i);
    const int n = (int)vis.size();
    int choice_at = -1;   // (the chosen one's place among them)
    for (int k = 0; k < n; k++)
        if (vis[k] == mods_choice) choice_at = k;
    int made = 0;
    // (the cards: the picture at the left as the game's save cards have it, the name, the author and category, the counts; the
    // installed ones tagged; the cards scrolled to keep the chosen one in view)
    const float card_h = 92 * s, pitch = 102 * s, pic_w = card_h * 16 / 9;
    const float view = bottom - list_top, total_h = n * pitch + (more || feed == mods::Feed::Loading ? 40 * s : 0);
    mods_page_rows = std::max(1, (int)(view / pitch) - 1);
    float& at = mods_scroll_px;
    float& want = mods_want_px;
    if (mods_wheel) want -= mods_wheel * pitch, mods_wheel = 0, mods_follow = false;
    if (total_h <= view || (mods_follow && choice_at < 0)) want = 0;
    else if (!mods_follow) want = std::clamp(want, 0.0f, total_h - view);
    else {
        const float y0 = choice_at * pitch - pitch * 0.5f, y1 = (choice_at + 1) * pitch + pitch * 0.5f;
        if (y0 < want) want = y0;
        if (y1 > want + view) want = y1 - view;
        want = std::clamp(want, 0.0f, total_h - view);
    }
    at += (want - at) * ease_step;
    if (std::fabs(want - at) < 0.5f) at = want;
    if (total_h > view) scroll_bar(c, cx0 + 12 * s, list_top, bottom, (int)total_h, (int)view, at);
    c.d->PushClipRect(ImVec2(left - 14 * s, list_top), ImVec2(right + 14 * s, bottom), true);
    std::vector<int> ask_thumbs;   // (asked for once the list is let go: the engine's calls lock it themselves)
    int ask_details = -1;
    textures_reap();
    for (int i = 0; i < n_all; i++) {   // (the pictures of another category's cards let go)
        if (list[i].texture && i != mods_choice && !mods_in_cat(list[i], mods_cat)) {
            texture_retire((ImTextureData*)list[i].texture);
            list[i].texture = nullptr, list[i].thumb_state = 0;
        }
    }
    for (int k = 0; k < n; k++) {
        const int i = vis[k];
        const ImVec2 a(left, list_top + k * pitch - at), b(right, a.y + card_h);
        // ((Triforce) a picture 12 cards or more out of view let go, the chosen one's kept: made again from its kept file
        // when it comes near. Kept all, the fifth page of mods used up the overlays' textures, 11-17)
        if ((b.y < list_top - 12 * pitch || a.y > bottom + 12 * pitch) && list[i].texture && i != mods_choice) {
            texture_retire((ImTextureData*)list[i].texture);
            list[i].texture = nullptr, list[i].thumb_state = 0;
        }
        if (b.y < list_top - pitch || a.y > bottom + pitch) continue;
        mods::Mod& m = list[i];
        if (!m.thumb_state) ask_thumbs.push_back(i);   // (the ones in view and the next few)
        if (b.y < list_top || a.y > bottom) continue;
        const bool sel = i == mods_choice;
        ui::plate(c, a, b, sel);
        const ImVec2 pa(a.x + 8 * s, a.y + 8 * s), pb(pa.x + pic_w - 16 * s, b.y - 8 * s);
        mods_picture(c, m, pa, pb, &made);
        const float tx = pb.x + 16 * s;
        float tag_w = 0;
        if (m.installed) tag_w = card_tag(c, b.x - 12 * s, a.y + 20 * s, m.update ? "Update" : "Installed") + 10 * s;
        c.d->PushClipRect(ImVec2(tx, a.y), ImVec2(b.x - 12 * s - tag_w, b.y), true);
        ui::text_mid(c, c.f.strong, 19 * s, tx, a.y + 22 * s, ui::kInkBright, m.name.c_str());
        c.d->PopClipRect();
        c.d->PushClipRect(ImVec2(tx, a.y), ImVec2(b.x - 12 * s, b.y), true);
        const std::string by = "by " + m.author + (m.category.empty() ? "" : "  \xC2\xB7  " + m.category);
        ui::text_mid(c, c.f.body, 15 * s, tx, a.y + 48 * s, ui::kSoft, by.c_str());
        const std::string counts = mods_count(m.likes) + " likes  \xC2\xB7  " + mods_count(m.views) + " views";
        ui::text_mid(c, c.f.body, 13.5f * s, tx, a.y + 70 * s, ui::alpha(ui::kSoft, 0.72f), counts.c_str());
        c.d->PopClipRect();
        if (sel) ui::glow(c, a, b);
        hits.add(a.x, std::max(a.y, list_top), b.x, std::min(b.y, bottom), menu_hit::kListLine, i);
    }
    if (n && (more || feed == mods::Feed::Loading)) {   // (the foot of the list: the next page comes when the choice is near it)
        const float y = list_top + n * pitch - at + 16 * s;
        if (y < bottom) ui::text_mid_centre(c, c.f.body, 15 * s, (left + right) * 0.5f, y, ui::kDim,
                                            feed == mods::Feed::Loading ? "Loading more..." : "More below");
    }
    c.d->PopClipRect();
    if (!n) {   // (no list yet: asking, or the answer that failed; (Triforce) a search's: asking, or none found)
        const float my = list_top + view * 0.38f, mx_ = (left + right) * 0.5f;
        std::string t = feed == mods::Feed::Failed ? "GameBanana could not be reached." : "Asking GameBanana for mods...";
        if (n_all && mods_cat) t = feed == mods::Feed::Loading || more ? "Looking for more in " + std::string(kModsCatNames[mods_cat]) + "..."
                                                                         : "None in " + std::string(kModsCatNames[mods_cat]) + " yet";
        if (!searched.empty() && feed != mods::Feed::Failed)
            t = feed == mods::Feed::Ready ? "No mods match \xE2\x80\x9C" + searched + "\xE2\x80\x9D" : "Searching for \xE2\x80\x9C" + searched + "\xE2\x80\x9D...";
        c.d->PushClipRect(ImVec2(left, list_top), ImVec2(right, bottom), true);
        ui::text_mid_centre(c, c.f.strong, 20 * s, mx_, my, ui::kInk, t.c_str());
        if (feed == mods::Feed::Failed) {
            ui::text_mid_centre(c, c.f.body, 15 * s, mx_, my + 34 * s, ui::kSoft, err.c_str());
            ui::text_mid_centre(c, c.f.body, 15 * s, mx_, my + 62 * s, ui::kAccent, "Press Confirm to try again");
        } else if (!searched.empty() && feed == mods::Feed::Ready) {
            ui::text_mid_centre(c, c.f.body, 15 * s, mx_, my + 34 * s, ui::kSoft, "Try another word, or Back for the whole list.");
        }
        c.d->PopClipRect();
    }
    mods_search_bar(c, ImVec2(left, top), ImVec2(right, top + search_h), searched, feed, n, more);   // (the lock held: no engine calls in it)
    // (the chosen mod at the right, on the caption plate: its picture large, what it is, its counts, the file, its description)
    const char* install_label = "Install";
    bool can_install = false;
    int files = 0;
    if (n && mods_choice >= 0 && choice_at >= 0) {
        mods::Mod& m = list[mods_choice];
        if (!m.details_state) ask_details = mods_choice;
        if (!m.thumb_state) ask_thumbs.push_back(mods_choice);
        files = (int)m.files.size();
        can_install = m.details_state == 2 && files > 0;
        install_label = m.update ? "Update" : m.installed ? "Reinstall" : "Install";
        const float x0 = hx0, x1 = hx1, pad = ui::lu(c, 64), y0 = top, y1 = bar - 8 * s;
        ui::box(c, ImVec2(x0, y0), ImVec2(x1, y1));
        c.d->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
        const float w = x1 - x0 - 2 * pad;
        float y = y0 + pad * 0.6f;
        const ImVec2 pa(x0 + pad, y), pb(x0 + pad + w, y + w * 9 / 16);
        mods_picture(c, m, pa, pb, &made);
        y = pb.y + 16 * s;
        y += ui::para(c, c.f.strong, 20 * s, ImVec2(x0 + pad, y), ui::kInk, m.name.c_str(), w) + 4 * s;
        const std::string by = "by " + m.author + (m.category.empty() ? "" : "  \xC2\xB7  " + m.category);
        ui::text(c, c.f.body, 15 * s, ImVec2(x0 + pad, y), ui::kAccent, by.c_str());
        y += 24 * s;
        std::string counts = mods_count(m.likes) + " likes  \xC2\xB7  " + mods_count(m.views) + " views";
        if (m.downloads >= 0) counts += "  \xC2\xB7  " + mods_count(m.downloads) + " downloads";
        ui::text(c, c.f.body, 14 * s, ImVec2(x0 + pad, y), ui::kSoft, counts.c_str());
        y += 24 * s;
        if (m.details_state == 2 && files) {
            const mods::File& f = m.files[std::clamp(m.file_choice, 0, files - 1)];
            std::string line = (files > 1 ? "File " + std::to_string(m.file_choice + 1) + " of " + std::to_string(files) + ":  " : std::string("File:  ")) +
                               f.name + "  (" + mods_size(f.bytes) + ")";
            ui::diamond(c, ImVec2(x0 + pad + 5 * s, y + 9 * s), 4 * s, ui::kGoldHi);
            y += ui::para(c, c.f.strong, 14 * s, ImVec2(x0 + pad + 16 * s, y), ui::kGoldHi, line.c_str(), w - 16 * s) + 2 * s;
            if (!f.description.empty()) y += ui::para(c, c.f.body, 13.5f * s, ImVec2(x0 + pad + 16 * s, y), ui::kSoft, f.description.c_str(), w - 16 * s);
            y += 8 * s;
        }
        c.d->AddLine(ImVec2(x0 + pad, y), ImVec2(x1 - pad, y), ui::alpha(ui::kEdge, 0.8f), std::max(1.0f, s));
        y += 12 * s;
        const char* text = m.details_state == 2 ? m.text.c_str() : m.details_state == 3 ? "The details could not be read from GameBanana." : "Loading the details...";
        if (m.details_state == 2 && !files) text = "This mod has no file to download on GameBanana.";
        {   // ((Triforce) 11-17) the description in its own window down to the panel's foot, scrolled by the right stick or the wheel
            const float dtop = y, dbot = y1 - pad * 0.6f;
            if (mods_desc_for != mods_choice) mods_desc_for = mods_choice, mods_desc_px = 0;
            const float th = ui::para(c, c.f.body, kHintPt * s, ImVec2(x0 + pad, dtop), ui::kInk, text, w, false);
            const float room = std::max(0.0f, dbot - dtop), most = std::max(0.0f, th - room);
            if (menu_pad_raw >> HOS_PAD_RS_DOWN & 1) mods_desc_px += 10 * s;
            if (menu_pad_raw >> HOS_PAD_RS_UP & 1) mods_desc_px -= 10 * s;
            if (mods_desc_wheel) mods_desc_px -= mods_desc_wheel * 60 * s, mods_desc_wheel = 0;
            mods_desc_px = std::clamp(mods_desc_px, 0.0f, most);
            c.d->PushClipRect(ImVec2(x0, dtop), ImVec2(x1, dbot), true);
            ui::para(c, c.f.body, kHintPt * s, ImVec2(x0 + pad, dtop - mods_desc_px), ui::kInk, text, w);
            c.d->PopClipRect();
            if (most > 0) scroll_bar(c, x1 - pad * 0.45f, dtop, dbot, (int)th, (int)room, mods_desc_px);
            mods_desc_x0 = x0;
        }
        c.d->PopClipRect();
    } else if (mods_choice < 0) {   // ((Triforce) the search bar chosen)
        caption(c, hx0, hx1, "Search", "Search GameBanana's mods for this game: type a word (a name, a character, an item, an "
                "author), or press Confirm for a keyboard. The list shows what matches as you type; Back shows the whole list again.",
                nullptr, nullptr);
    } else {
        caption(c, hx0, hx1, "Mods", "Mods for this game from GameBanana: choose one to see what it does, then install it. It is "
                "in the game at its next start.", nullptr, nullptr);
    }
    mods::unlock();
    if (ask_details >= 0) mods::want_details(ask_details);
    for (int i : ask_thumbs) mods::want_thumb(i);
    if (salesman_on) {   // (him, and a line now and then: a new one when the choice moves, not more than every 9 s)
        const float sy0 = bottom + 10 * s, sy1 = bar - 8 * s;
        salesman_picture(c, ImVec2(left, sy0), ImVec2(left + 170 * s, sy1));
        const int64_t now = prof_now();
        static int last_choice = -1;
        if (last_choice != mods_choice && now - salesman_line_at > 9000000000LL) {
            last_choice = mods_choice, salesman_line_at = now;
            salesman_line = (salesman_line + 1) % (int)(sizeof salesman_lines / sizeof *salesman_lines);
        }
        if (salesman::state() == salesman::State::Ready && inst.job == mods::Job::None)
            salesman_says(c, left + 178 * s, right, bar - 14 * s, salesman_lines[salesman_line]);
    }
    // (the install under way, or its end: over the page)
    if (inst.job == mods::Job::Downloading || inst.job == mods::Job::Unpacking) {
        static const char* const labels[] = {"Cancel"};
        mods_btn = 0;
        char left_t[96] = "", right_t[64] = "";
        float frac = 0;
        if (inst.job == mods::Job::Downloading) {
            frac = inst.total ? (float)inst.done / (float)inst.total : 0;
            snprintf(left_t, sizeof left_t, "Downloading...  %s / %s", mods_size(inst.done).c_str(), inst.total ? mods_size(inst.total).c_str() : "?");
            if (inst.rate > 1024 && inst.total > inst.done) {
                const int secs = (int)((inst.total - inst.done) / inst.rate);
                snprintf(right_t, sizeof right_t, "%.0f%%  \xC2\xB7  about %d:%02d left", frac * 100, secs / 60, secs % 60);
            } else snprintf(right_t, sizeof right_t, "%.0f%%", frac * 100);
        } else {
            frac = 1;
            snprintf(left_t, sizeof left_t, "Unpacking...");
        }
        mods_dialog(c, ("Installing " + inst.mod_name).c_str(), inst.file_name, labels, 1, frac, left_t, right_t);
        const ui::Prompt p[] = {{ui::kBack, "Cancel"}};
        ui::prompt_bar(c, p, 1);
        return;
    }
    if (inst.job == mods::Job::Done) {
        static const char* const labels[] = {"Restart Now", "Later"};
        mods_dialog(c, "Installed!", inst.mod_name + "\n" + inst.message + "\nRestart the game to play with it.", labels, 2, 1, "Installed", "Restart to apply");
        const ui::Prompt p[] = {{ui::kUpDown, "Move"}, {ui::kConfirm, "Choose"}, {ui::kBack, "Later"}};
        ui::prompt_bar(c, p, 3);
        return;
    }
    if (inst.job == mods::Job::Failed || inst.job == mods::Job::Cancelled) {
        static const char* const labels[] = {"OK"};
        mods_btn = 0;
        mods_dialog(c, inst.job == mods::Job::Cancelled ? "Cancelled" : "Not installed", inst.mod_name + "\n" + inst.message, labels, 1);
        const ui::Prompt p[] = {{ui::kConfirm, "OK"}};
        ui::prompt_bar(c, p, 1);
        return;
    }
    if (mods_kbd) {   // ((Triforce) the on-screen keyboard, over the page)
        mods_keyboard(c);
        const ui::Prompt p[] = {{ui::kUpDown, "Move"}, {ui::kConfirm, "Type"}, {ui::kBack, mods_query.empty() ? "Close" : "Delete"}};
        ui::prompt_bar(c, p, 3);
        return;
    }
    ui::Prompt prompts[7];
    int pn = 0;
    const bool clears = !searched.empty() || !mods_query.empty();
    prompts[pn++] = {ui::kUpDown, "Move"};
    prompts[pn++] = {ui::kTabs, "Category"};
    if (mods_choice < 0)
        prompts[pn++] = {ui::kConfirm, mods_query != mods_query_sent ? "Search" : !n && feed == mods::Feed::Failed ? "Try again"
                                       : pad_last || !n ? "Keyboard" : "Results"};
    else {
        if (files > 1) prompts[pn++] = {ui::kLeftRight, "File"};
        if (mods_query != mods_query_sent) prompts[pn++] = {ui::kConfirm, "Search"};
        else if (can_install) prompts[pn++] = {ui::kConfirm, install_label};
        else if (!n && feed == mods::Feed::Failed) prompts[pn++] = {ui::kConfirm, "Try again"};
    }
    prompts[pn++] = {ui::kBack, clears ? "Clear search" : "Back"};
    ui::prompt_bar(c, prompts, pn);
}

/// (Triforce) The Mods entry, under the game list's last button by the list's own pitch (host/oot_menu.c): on the pause menu's
/// System page a button as the game's own (PaCommonBtnThin: the plate with the curls, 672 x 144 units, its text 54; the
/// cursor round it when chosen), on the title its line of text as the game sets the list there (the chosen line on a dark
/// band, its text glowing blue). c: the 16:9 column, where the layout's units are
void draw_mods_row(ui::Ctx& c, bool sel) {
    const float x0 = (float)mods_box[0].load(std::memory_order_relaxed), x1 = (float)mods_box[1].load(std::memory_order_relaxed);
    const float y0 = (float)mods_box[2].load(std::memory_order_relaxed), y1 = (float)mods_box[3].load(std::memory_order_relaxed);
    const float pitch = (float)mods_box[4].load(std::memory_order_relaxed);
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f - pitch;
    const bool title = (mods_list_bits.load(std::memory_order_relaxed) & HOS_MODS_LIST_TITLE) != 0;
    const ImVec2 m = ui::at(c, cx, cy);
    if (!title) {
        const ImVec2 a = ui::at(c, cx - 336, cy + 72), b = ui::at(c, cx + 336, cy - 72);
        ui::button(c, a, b, "Mods", sel);
        hits.add(a.x, a.y, b.x, b.y, menu_hit::kButton, 0);
        return;
    }
    const float px = ui::lu(c, 78);
    if (sel) {   // (the game's chosen line: a dark band fading out at its ends, the text glowing blue-white)
        // ((Triforce) 11-17, his "it doesn't have the black underneath the lettering": the game's own band, as the lab's 1080p
        // shot of its chosen Options line measures it: about 470 wide and 58 high, near black, solid but for its last ~60 at
        // each end. It was 1120 x 100 at alpha 150 fading from the middle: too faint to see. In the layout's units, about 0.45
        // of a 1080p pixel: the lab's shot of a first try of 524 units was 215 px wide)
        const float core = ui::lu(c, 400), hw = ui::lu(c, 522), hh = ui::lu(c, 64);
        const ImU32 dark = ui::rgba(8, 6, 12, 205), none = ui::rgba(8, 6, 12, 0);
        c.d->AddRectFilled(ImVec2(m.x - core, m.y - hh), ImVec2(m.x + core, m.y + hh), dark);
        c.d->AddRectFilledMultiColor(ImVec2(m.x - hw, m.y - hh), ImVec2(m.x - core, m.y + hh), none, dark, dark, none);
        c.d->AddRectFilledMultiColor(ImVec2(m.x + core, m.y - hh), ImVec2(m.x + hw, m.y + hh), dark, none, none, dark);
        for (int k = 0; k < 12; k++) {
            const float ang = k * 0.5236f, r = ui::lu(c, 7) * (1 + 0.4f * c.pulse);
            ui::text_mid_centre(c, c.f.body, px, m.x + std::cos(ang) * r, m.y + std::sin(ang) * r, ui::rgba(120, 140, 255, 26), "Mods");
        }
        ui::text_mid_centre(c, c.f.body, px, m.x, m.y, ui::rgba(214, 222, 255), "Mods");
    } else {   // (as the game's other lines: white with a soft dark edge)
        for (int k = 0; k < 8; k++) {
            const float ang = k * 0.785398f, r = ui::lu(c, 3);
            ui::text_mid_centre(c, c.f.body, px, m.x + std::cos(ang) * r, m.y + std::sin(ang) * r + ui::lu(c, 2), ui::rgba(20, 14, 6, 90), "Mods");
        }
        ui::text_mid_centre(c, c.f.body, px, m.x, m.y, ui::rgba(250, 248, 240), "Mods");
    }
    const float hw = ui::lu(c, 400), hh = ui::lu(c, pitch * 0.5f);
    hits.add(m.x - hw, m.y - hh, m.x + hw, m.y + hh, menu_hit::kButton, 0);
}

/// (the entry while the menu is shut: in the 16:9 column, as draw_game_tab places its tabs)
void draw_mods_row_shut(ui::Ctx& c) {
    ui::Ctx k = c;
    k.w = std::min(c.w, std::floor(c.h * 16.0f / 9.0f + 0.5f));
    const float ox = std::floor((c.w - k.w) * 0.5f);
    const int vtx0 = c.d->VtxBuffer.Size;
    const menu_hit::Map saved = hits;   // (its hit is not the menu's: the host takes clicks on it while the menu is shut)
    draw_mods_row(k, false);
    hits = saved;
    for (int i = vtx0; i < c.d->VtxBuffer.Size; i++) c.d->VtxBuffer[i].pos.x += ox;
}

void draw_ask(ui::Ctx& c) {
    const float s = c.s;
    c.d->AddRectFilled(ImVec2(0, 0), ImVec2(c.w, c.h), ui::rgba(8, 5, 2, 110));   // (the dialog over the page: darker still)
    ui::banner(c, 0, "Restart Needed");
    const std::string ask = "Restart to apply " + restart_list(" and ") + "?";
    const float wrap = ui::lu(c, 1440), px = ui::lu(c, 54 * ui::kType);
    const float th = ui::para(c, c.f.body, px, ImVec2(), 0, ask.c_str(), wrap, false);
    float y = ui::at(c, 0, 250).y - th * 0.5f;
    {   // (each line centred, as the game's dialogs set their text)
        const char* t = ask.c_str();
        const char* end = t + ask.size();
        while (t < end) {
            const char* cut = c.f.body->CalcWordWrapPosition(px, t, end, wrap);
            if (cut == t) cut = t + 1;
            const float lw = c.f.body->CalcTextSizeA(px, FLT_MAX, 0, t, cut).x;
            c.d->AddText(c.f.body, px, ImVec2(c.w * 0.5f - lw * 0.5f, y), ui::kInk, t, cut);
            y += px * 1.3f;
            t = cut;
            while (t < end && (*t == ' ' || *t == '\n')) t++;
        }
    }
    ui::text_mid_centre(c, c.f.body, ui::lu(c, 52), c.w * 0.5f, y + 16 * s, ui::kSoft, "Progress since your last save is lost.");
    // (SystemWindow_00's buttons: PaCommonBtnThin's plate, 684 x 156 units, 188 apart; the upper one acts, the lower one backs out)
    const float bw = ui::lu(c, 336), bh = ui::lu(c, 72);
    const char* labels[2] = {"Later", "Restart Now"};
    for (int i = 0; i < 2; i++) {
        const float my = ui::at(c, 0, i ? -330.0f : -518.0f).y;
        const ImVec2 a(c.w * 0.5f - bw, my - bh), b(c.w * 0.5f + bw, my + bh);
        ui::button(c, a, b, labels[i], i == ask_sel);
        hits.add(a.x, a.y, b.x, b.y, menu_hit::kButton, i);
    }
    const ui::Prompt prompt_list[] = {{ui::kConfirm, "Confirm"}, {ui::kBack, "Later"}};
    ui::prompt_bar(c, prompt_list, 2);
}

void draw_menu(ui::Ctx& c, bool perf_under = false);
void draw_perf(ui::Ctx& c);
/// The menu's text sizes at this window's unit (and, with Menu size Auto, at the other size Auto can pick), baked ahead by
/// vk_imgui.cpp a few a frame: the first open then draws with no glyphs to make (it took 2.6 ms at 1080p)
float prebaked[4] = {0, 0, 0, 0};   // (the units and the width prebake last did; (Triforce) and the Full panel's text size, 0 off)
/// The menu's units now (with Auto, both it can pick) for a window w x h: whether prebake has not done them yet
bool prebake_needed(uint32_t w, uint32_t h) {
    float m = 0;
    const float u0 = ui::unit((float)h, opts.menu_size, opts.fullscreen != 0, auto_pad, &m);
    const float u1 = opts.menu_size == 0 ? ui::unit((float)h, 0, opts.fullscreen != 0, !auto_pad, &m) : u0;
    return u0 != prebaked[0] || u1 != prebaked[1] || (float)w != prebaked[2];
}
/// (Triforce) The Full panel's glyphs at its one text size px, in the font draw_perf writes with, baked now: ui::prebake only
/// queues a size for the next begin (vk_imgui.cpp bake_some), which bakes printable ASCII alone, never the temperatures'
/// degree sign, so the panel's first frame at a new size would make its glyphs while it draws. Again for a new size (a
/// resize, a new DPI) or a new font (the game's, once read from the player's files); px 0: off, the next Full bakes anew
void bake_perf(const ui::Ctx& c, float px) {
    static const ImFont* done_font = nullptr;
    static float done_px = 0;
    ImFont* font = c.f.body ? c.f.body : c.f.strong;   // (draw_perf's)
    if (px <= 0) done_px = 0;
    if (px <= 0 || !font || (font == done_font && px == done_px)) return;
    done_font = font, done_px = px;
    ImFontBaked* b = font->GetFontBaked(px);
    for (ImWchar ch = 32; ch < 127; ch++) b->FindGlyph(ch);
    b->FindGlyph(0x00B0);   // (U+00B0: the temperatures' degree sign)
}
void prebake(const ui::Ctx& c) {
    float units[2] = {c.s, c.s}, margins[2] = {c.safe, c.safe};
    if (opts.menu_size == 0) units[1] = ui::unit(c.h, 0, opts.fullscreen != 0, !auto_pad, &margins[1]);
    // ((Triforce) the Full panel's one text size, as draw_perf sizes it, while the row is on Full: baked here at once when it
    // changes (bake_perf), before draw_perf in the same frame, and kept by ui::prebake with the menu's sizes. Its first
    // frame, and one after a resize, then make no glyphs while drawing)
    const float perf = opts.show_fps == 2 ? perf_px(perf_scale(c.w, c.h)) : 0;
    bake_perf(c, perf);
    if (units[0] == prebaked[0] && units[1] == prebaked[1] && c.w == prebaked[2] && perf == prebaked[3]) return;
    prebaked[0] = units[0], prebaked[1] = units[1], prebaked[2] = c.w, prebaked[3] = perf;
    static const float points[] = {12, 12.5f, 13, 13.6f, 14, 14.28f, 14.5f, 15, 15.5f, 16, 16.5f, 17, 18, 18.5f, 19, 20, 21, 23, 24};
    constexpr int n = sizeof points / sizeof points[0];
    float px[2 * n + 1];
    for (int u = 0; u < 2; u++)
        for (int i = 0; i < n; i++) px[u * n + i] = points[i] * units[u];
    int baked = units[1] == units[0] ? n : 2 * n;
    if (perf > 0) px[baked++] = perf;
    ui::prebake(px, baked);
    if (open_now.load(std::memory_order_relaxed)) return;
    // (and the page itself drawn once into a list of its own, thrown away: what the first open would measure and bake (the
    // help's height over every row, the exact sizes) done now, the menu's state as it was)
    const menu_hit::Map saved_hits = hits;
    const int saved_marquee = marquee_max;
    const int saved_tele[2] = {teleport_columns, teleport_page_rows}, saved_inv[3] = {inventory_columns, inventory_capacity, inventory_page};
    const float saved_tele_px[2] = {teleport_scroll_px, teleport_want_px};
    const float saved_ease = ease_step;
    float saved_scroll[sizeof scroll_at / sizeof *scroll_at], saved_want[sizeof scroll_at / sizeof *scroll_at];
    int saved_to[sizeof scroll_at / sizeof *scroll_at];   // (the scratch drawing's scroll, given back: the next is the menu's)
    memcpy(saved_scroll, scroll_at, sizeof scroll_at);
    memcpy(saved_want, scroll_want, sizeof scroll_want);
    memcpy(saved_to, scrolled_to, sizeof scrolled_to);
    ImDrawList scratch(ImGui::GetDrawListSharedData());
    scratch._ResetForNewFrame();
    scratch.PushTexture(ImGui::GetIO().Fonts->TexRef);
    scratch.PushClipRectFullScreen();
    ease_step = 0;
    for (int u = 0; u < (units[1] == units[0] ? 1 : 2); u++) {
        ui::Ctx dry = c;
        dry.d = &scratch;
        dry.s = units[u], dry.safe = margins[u];
        draw_menu(dry);
    }
    hits = saved_hits, marquee_max = saved_marquee, ease_step = saved_ease;
    teleport_columns = saved_tele[0], teleport_page_rows = saved_tele[1], teleport_scroll_px = saved_tele_px[0], teleport_want_px = saved_tele_px[1];
    inventory_columns = saved_inv[0], inventory_capacity = saved_inv[1], inventory_page = saved_inv[2];
    memcpy(scroll_at, saved_scroll, sizeof scroll_at);
    memcpy(scroll_want, saved_want, sizeof scroll_want);
    memcpy(scrolled_to, saved_to, sizeof scrolled_to);
}

float menu_ox = 0;   // (the menu's column's left edge in the window: more than 0 only on a window wider than 16:9)
/// The menu: the game dimmed across the whole window, the pages in a column as wide as a 16:9 screen of this height, in the
/// middle (a window of 16:9 or narrower: the whole of it). The pages draw from 0 as on 16:9; their vertices and clip
/// rectangles are moved over after, and menu_tick moves the mouse back by menu_ox before it tests the hits. (Triforce)
/// perf_under: the FPS counter row's Full panel, over the dim and under the pages, as NX2navi keeps its panel under its menu
void draw_menu(ui::Ctx& c, bool perf_under) {
    const bool row = page == kModsRowPage;   // ((Triforce) the Mods entry has the cursor: over the game's list, nothing dimmed)
    if (!embed_open && !row) c.d->AddRectFilled(ImVec2(0, 0), ImVec2(c.w, c.h), ui::rgba(8, 5, 2, ui::kDimAlpha));   // (the game behind, dimmed)
    if (perf_under) draw_perf(c);   // (before the pages' first vertex: the window's top right, not moved with the column)
    ui::Ctx k = c;
    k.over_game = embed_open;   // (inside the game's own Options screen: its background stays, its list is under our plates)
    k.w = std::min(c.w, std::floor(c.h * 16.0f / 9.0f + 0.5f));
    menu_ox = std::floor((c.w - k.w) * 0.5f);
    const int vtx0 = c.d->VtxBuffer.Size, cmd0 = c.d->CmdBuffer.Size;
    hits.clear(0, 0, k.w, k.h);
    hits_game_tab = false;
    if (row) draw_mods_row(k, true);
    else if (page != 5) {   // (the System screens' dark column behind the list: SystemBg_05, under the header)
        float cx0, cx1, hx0, hx1;
        bool narrow;
        columns(k, &cx0, &cx1, &hx0, &hx1, &narrow);
        ui::column_bg(k, cx0, cx1);
    }
    if (row) {}   // (drawn above)
    else if (page == 5) draw_ask(k);
    else if (page == 1) draw_controls(k);
    else if (page == 3) draw_teleport(k);
    else if (page == 4) draw_inventory(k);
    else if (page == 6) draw_names(k);   // ((Triforce) Game flags, Cutscenes, Scan amiibo)
    else if (page == kModsPage) draw_mods(k);
    else draw_options(k);
    if (menu_ox <= 0) return;
    for (int i = vtx0; i < c.d->VtxBuffer.Size; i++) c.d->VtxBuffer[i].pos.x += menu_ox;
    // (a clip rectangle of the column's: moved with it; the whole window's (the dim's, and the draw list's own) stays, as it
    // holds the column wherever it is)
    for (int i = std::max(0, cmd0 - 1); i < c.d->CmdBuffer.Size; i++) {
        ImVec4& r = c.d->CmdBuffer[i].ClipRect;
        if (r.x <= 0 && r.z >= c.w) continue;
        r.x += menu_ox, r.z += menu_ox;
    }
    // (the draw list's current header, for whatever is drawn next: the window's again after the pages' pops)
}

/// The FPS counter, as a counter of the game's HUD: the rate in the HUD's own digits (NinDev-Number, the rupee counter's
/// font), white with the HUD's soft dark edge, "FPS" small beside it in the menus' font; with frame generation the game's
/// own rate after a slash, smaller, as the HUD writes an amount of its most ("12 / 99"). No plate: the HUD has none.
/// Where: at the top, its right end 500 units left of the screen's right edge, its middle 60 units down. Measured on the
/// game's own screens (1920 x 1080), that strip is free in play and in the menus alike: left of the item box, its page dots
/// and R key (from 1725 px) and above the fishing slots (from 53 px down); in the pause menu between the tabs' R icon
/// (to 1470 px) and the rupee counter (from 1715 px); right of the hearts and the mini-game timer. Under the header, at
/// the right, while our own options are open (their tabs' R icon is in that strip)
/// The HUD's soft dark edge under a text, then the text, its top left at p: the text eight times a little to each side of
/// itself and down, dark and half seen, e (px / 20, a pixel at least) from it. The FPS counter's, and (Triforce) the Full
/// panel's texts', so that they read over a bright scene and the HUD's own art
void edged_text(ui::Ctx& c, ImFont* f, float px, ImVec2 p, ImU32 col, const char* t) {
    const float e = std::max(1.0f, px * 0.05f);
    for (int k = 0; k < 8; k++) {
        const float ang = k * 0.785398f;
        c.d->AddText(f, px, ImVec2(p.x + std::cos(ang) * e, p.y + std::sin(ang) * e + e * 0.6f), ui::rgba(20, 14, 6, 150), t);
    }
    c.d->AddText(f, px, p, col, t);
}

void draw_fps(ui::Ctx& c, bool under_banner) {
    if (!fps_text[0]) return;
    ImFont* digits = c.f.number ? c.f.number : c.f.strong;
    const float big = ui::lu(c, 72), small = ui::lu(c, 46), label = ui::lu(c, 34), gap = ui::lu(c, 12);
    const char* game = fps_sub[0] ? fps_sub : nullptr;   // ("60": the game's own rate, with frame generation)
    const float nw = ui::size_of(digits, big, fps_text).x, lw = ui::size_of(c.f.strong, label, "FPS").x;
    const float sw = game ? ui::size_of(digits, small, "/").x + ui::lu(c, 8) + ui::size_of(digits, small, game).x + gap : 0;
    const float total = nw + gap + sw + lw;
    const bool below = under_banner && page != 5;   // (our options open: under their header)
    // (a window wider than 16:9: the HUD keeps its 16:9 shape in the middle, and so does the counter)
    const float hud_w = std::min(c.w, std::floor(c.h * 16.0f / 9.0f + 0.5f)), hud_x1 = (c.w + hud_w) * 0.5f;
    const float x1 = below ? hud_x1 - ui::lu(c, 80) : hud_x1 - ui::lu(c, 500);
    const float my = below ? banner_h(c) + ui::lu(c, 50) : ui::lu(c, 60);
    auto edged = [&](ImFont* f, float px, float x, float y, const char* t, ImU32 col) {   // (the HUD's dark edge: edged_text, its middle at y)
        edged_text(c, f, px, ImVec2(x, ui::mid_top(f, px, y)), col, t);
        return ui::size_of(f, px, t).x;
    };
    float x = x1 - total;
    x += edged(digits, big, x, my, fps_text, ui::kInk) + gap;
    if (game) {
        x += edged(digits, small, x, my + ui::lu(c, 8), "/", ui::kSoft) + ui::lu(c, 8);
        x += edged(digits, small, x, my + ui::lu(c, 8), game, ui::kSoft) + gap;
    }
    edged(c.f.strong, label, x, my + ui::lu(c, 12), "FPS", ui::kAccent);
}

/// (Triforce) The performance overlay on Full: NX2navi's full panel, laid out as its own is, to the pixel of its scale
/// ((Triforce: from NX2navi runtime/ui/perf_panel.cpp perf_panel_draw, machine_and_frames, frame_plot, waits, picture and
/// divider, and runtime/ui/menu.cpp menu_scale, at 62cc3b13; MIT, the notice in perf_machine.h). Drawn with Dear ImGui's draw
/// list in place of its window and tables, by the metrics those had there, in pixels at its scale (the window's lines over
/// 1080; on a window taller than 0.68 of its width, that width's; 720 lines at the display's scale at least): the window's
/// padding 24 x 20, the cells' 7 x 1.5, items 12 apart, the text its menu font's 25.5 at the window's 0.72, a divider 26 x
/// 0.72 high, the graph 48 high inside its frame's padding of 14 x 8, its line a pixel wide. Top to bottom: the title (perf_title: this build of Triforce and the game's version); a divider; the
/// machine (GPU, VRAM, CPU, RAM); a divider; the frames (FPS with its 1% and 0.1% lows, FG while frame generation makes
/// frames, Frame), the two in a name column and three right-aligned columns, each as wide as the widest text it can hold
/// (NX2navi's samples of 8s), so the panel keeps its width as the numbers change; the newest kPerfGraph frame times as a line
/// over a dark band, 0 at the bottom, 50 ms or the longest of them at the top, frames not seen yet 0; the figures per frame;
/// a divider; the settings that shape the picture (a row without a value left out); a divider and the GPU's and the CPU's names.
/// Themed as the options are, with what they draw with from the player's own game files at run time (vk_game_tex.h): the
/// panel is the options' caption plate (SystemCaptionBg_00, ui::box) at kPerfOpacity, so the game shows through it; each
/// divider the options' separator (OptionSeparateMark_00 and SystemLine_03, ui::separator) at NX2navi's divider's size, in its
/// divider's tints; the text in the options' own font (the game's "Normal", menu_ctx), the names in the options' gold
/// (ui::kAccent), the values in their white (ui::kInk), the title, the GPU's and CPU's names and a "-" where the machine gives
/// no number in their soft white (ui::kSoft), each text with the FPS counter's thin dark edge (edged_text) so that it reads
/// through the plate over a bright scene. Where the game's files have none of it, ui::box and ui::separator draw their plain
/// plates. Where: NX2navi's, the window's top right corner 12 in from both edges, at every shape of window; Full draws no FPS
/// counter (menu_record), and while the options are open the panel is under them, over their dim (draw_menu), as NX2navi's
/// is under its menu's window. The scale is NX2navi's menu_scale with its DPI floor (perf_scale), at which prebake has baked
/// the text's size. perf_sample made the texts
void draw_perf(ui::Ctx& c) {
    const PerfView& v = perf_view;
    if (!v.have) return;
    ImFont* font = c.f.body ? c.f.body : c.f.strong;
    if (!font) return;
    const float k = perf_scale(c.w, c.h);   // (NX2navi's menu_scale, its DPI floor too: the size prebake baked)
    const float px = perf_px(k), cell_y = 1.5f * k, gap = 2 * 7.0f * k, item = 12 * k, row = px + 2 * cell_y;
    const float pad_x = 24 * k, pad_y = 20 * k, ds = 0.72f * k, div_h = 26 * ds, graph_h = 48 * k;
    const ImU32 label_col = ui::kAccent, value_col = ui::kInk, dim_col = ui::alpha(ui::kSoft, 0.8f);
    const auto tw = [&](const char* t) { return ui::size_of(font, px, t).x; };
    // (the columns of the machine and the frames)
    float col[4] = {tw("-"), 0, 0, 0};
    for (const char* n : kPerfTop) col[0] = std::max(col[0], tw(n));
    static const char* const samples[3][5] = {{"100%", "888.8 GB", "888", "888.8 ms", ""},
                                              {"188 \xC2\xB0" "C", "/ 888.8 GB", "1% low 888", "max 8888.8 ms", ""},
                                              {"", "8888 MHz", "game 100%", "game 888.8 GB", "0.1% low 888"}};
    for (int j = 0; j < 3; j++) {
        for (const char* s : samples[j]) col[j + 1] = std::max(col[j + 1], tw(s));
        for (int r = 0; r < kPerfTopRows; r++) col[j + 1] = std::max(col[j + 1], tw(v.top[r][j].t));   // (a longer text all the same)
    }
    float wait_name = 0, wait_value = std::max(tw("888.88 ms"), tw("888.8 per frame")), set_name = 0, set_value = 0;
    for (int i = 0; i < kPerfFrameRows; i++) wait_name = std::max(wait_name, tw(kPerfFrame[i])), wait_value = std::max(wait_value, tw(v.frame[i].t));
    int settings = 0;
    for (int i = 0; i < kPerfSettings; i++)
        if (v.setting[i][0]) settings++, set_name = std::max(set_name, tw(kPerfSetting[i])), set_value = std::max(set_value, tw(v.setting[i]));
    const float inner = std::ceil(std::max({col[0] + col[1] + col[2] + col[3] + 3 * gap, wait_name + gap + wait_value,
                                            set_name + gap + set_value, tw(v.title), tw(v.gpu_name), tw(v.cpu_name)}));
    const bool names = v.gpu_name[0] || v.cpu_name[0];
    const float content = px + item + div_h + item + 4 * row + item + div_h + item + (v.fg ? 3 : 2) * row + item + 4 * k + item +
                          graph_h + item + 2 * k + item + kPerfFrameRows * row + item + div_h + item + settings * row +
                          (names ? item + div_h + item + (v.gpu_name[0] ? px : 0) + (v.gpu_name[0] && v.cpu_name[0] ? item : 0) +
                                       (v.cpu_name[0] ? px : 0)
                                 : 0);
    // (the panel: the window's top right corner, 12 in from both edges at every shape of window, as NX2navi's
    // perf_panel_draw; Full draws no FPS counter beside it, as NX2navi's perf_draw)
    const float m = 12 * k, panel_w = std::ceil(inner + 2 * pad_x), panel_h = std::ceil(content + 2 * pad_y);
    const ImVec2 a(std::floor(c.w - m - panel_w), std::floor(m)), b(a.x + panel_w, a.y + panel_h);
    ui::box(c, a, b, kPerfOpacity);
    // (the contents, from the padding in)
    const float x0 = a.x + pad_x, x1 = x0 + inner;
    float y = a.y + pad_y;
    ui::Ctx dc = c;
    dc.s = 0.7035f * k;   // (the separator's ornament (342 layout units) as wide as NX2navi's divider's: 111 of its scale's 0.72)
    const auto divider = [&] {
        ui::separator(dc, x0 - 4 * ds, x1 + 4 * ds, std::floor(y + div_h * 0.5f) + 0.5f, ui::rgba(196, 164, 112), ui::rgba(150, 126, 90));
        y += div_h + item;
    };
    // (every text with the FPS counter's thin dark edge, edged_text: it reads over a bright scene or the HUD's art through the
    // see-through plate)
    const auto text = [&](ImVec2 p, ImU32 col, const char* t) { edged_text(c, font, px, p, col, t); };
    const auto label = [&](const char* t) { text(ImVec2(x0, y + cell_y), label_col, t); };
    const auto right = [&](float x_end, const char* t, bool dim) {
        if (t[0]) text(ImVec2(x_end - tw(t), y + cell_y), dim ? dim_col : value_col, t);
    };
    const auto table = [&](int r0, int r1) {   // (the top block's rows r0 to r1: a name and three cells)
        for (int r = r0; r < r1; r++) {
            if (r == kPerfFG && !v.fg) continue;
            label(kPerfTop[r]);
            float x = x0 + col[0];
            for (int j = 0; j < 3; j++) x += gap + col[j + 1], right(x, v.top[r][j].t, v.top[r][j].dim);
            y += row;
        }
        y += item;
    };
    text(ImVec2(x0, y), dim_col, v.title);   // (the build first: a screenshot in a bug report carries it)
    y += px + item;
    divider();
    table(0, 4);
    divider();
    table(4, kPerfTopRows);
    y += 4 * k + item;
    {   // (the graph: NX2navi's PlotLines, its frame black at 0.32, the line in the names' gold)
        static float ms[kPerfGraph];
        int n;
        {
            std::lock_guard lk(perf_mu);
            n = perf_stats::newest(perf_ring, ms, kPerfGraph);
        }
        const ImVec2 g0(x0, y), g1(x1, y + graph_h), p0(g0.x + 14 * k, g0.y + 8 * k), p1(g1.x - 14 * k, g1.y - 8 * k);
        c.d->AddRectFilled(g0, g1, ui::rgba(0, 0, 0, 82));
        float most = 50;
        for (int i = 0; i < n; i++) most = std::max(most, ms[i]);
        static ImVec2 pts[kPerfGraph];
        for (int i = 0; i < kPerfGraph; i++) {   // (the oldest at the left; before the first frames, 0)
            const float t = i < kPerfGraph - n ? 0.0f : ms[i - (kPerfGraph - n)];
            pts[i] = ImVec2(p0.x + (p1.x - p0.x) * (float)i / (kPerfGraph - 1), p1.y - std::min(t, most) / most * (p1.y - p0.y));
        }
        c.d->AddPolyline(pts, kPerfGraph, label_col, ImDrawFlags_None, 1.0f);   // (PlotLines' line: 1 pixel at every scale)
    }
    y += graph_h + item + 2 * k + item;
    for (int i = 0; i < kPerfFrameRows; i++) label(kPerfFrame[i]), right(x1, v.frame[i].t, v.frame[i].dim), y += row;
    y += item;
    divider();
    for (int i = 0; i < kPerfSettings; i++)
        if (v.setting[i][0]) label(kPerfSetting[i]), right(x1, v.setting[i], false), y += row;
    if (names) {
        y += item;
        divider();
        if (v.gpu_name[0]) text(ImVec2(x0, y), dim_col, v.gpu_name), y += px + item;
        if (v.cpu_name[0]) text(ImVec2(x0, y), dim_col, v.cpu_name);
    }
}

/// (Triforce) The game's own tab inside its Options screen: our header over the screen's own (the same picture), the tabs on
/// it with "Game" chosen; the rest is the game's. It comes in over the screen's opening and goes with its closing
void draw_game_tab(ui::Ctx& c) {
    ui::Ctx k = c;
    k.w = std::min(c.w, std::floor(c.h * 16.0f / 9.0f + 0.5f));
    const float ox = std::floor((c.w - k.w) * 0.5f);
    const int vtx0 = c.d->VtxBuffer.Size;
    ui::banner(k, 0);
    // ((Triforce) the tabs as the mouse takes them, as draw_menu's are: their places in k's pixels, and the shift right after)
    hits.clear(0, 0, k.w, k.h);
    game_tab_ox = ox, hits_game_tab = true;
    draw_tabs(k, -1, true, true);
    static const bool still = getenv("RX_VK_UI_STILL") != nullptr;
    const int64_t now = prof_now(), seen = game_options_ns.load(std::memory_order_relaxed);
    float f = 1;
    if (!still) {
        f = std::clamp((float)((now - embed_since - 120000000LL) / 180e6), 0.0f, 1.0f);              // (in: after the screen's first frames)
        f *= 1 - std::clamp((float)((now - seen - 60000000LL) / 150e6), 0.0f, 1.0f);                  // (out: once it no longer runs)
    }
    for (int i = vtx0; i < c.d->VtxBuffer.Size; i++) {
        ImDrawVert& v = c.d->VtxBuffer[i];
        v.pos.x += ox;
        if (f < 1) v.col = ui::alpha(v.col, f);
    }
}

/// (Triforce) A line at the foot of the screen for a moment, as a guide of the game's: the Debug tab unlocked or locked
void draw_notice(ui::Ctx& c) {
    const float my = ui::at(c, 0, -1018).y, right = ui::at(c, 1840, 0).x, px = ui::lu(c, 48);
    const float w = ui::size_of(c.f.body, px, notice_text).x;
    if (const ui::Art* t = ui::art("SystemGuideBase_00")) ui::image(c, t, ImVec2(right - w - ui::lu(c, 300), ui::prompt_bar_top(c)), ImVec2(c.w + ui::lu(c, 4), c.h), ui::rgba(0, 0, 0, 215), true);
    else c.d->AddRectFilled(ImVec2(right - w - ui::lu(c, 60), my - px), ImVec2(c.w, c.h), ui::kBar);
    ui::text_mid(c, c.f.body, px, right - w, my, ui::kInk, notice_text);
}

/// The start-up screen: the logo, the wordmark, what the start is doing, a bar, on a warm dark. A little below the middle; what
/// is under the logo at 0.86 of the logo's scale. startup_fade, 0 to 1, darkens it all to black as it ends
void draw_startup(ui::Ctx& c) {
    StartupView v;
    {
        std::lock_guard lk(startup_mu);
        v = startup;
    }
#if __has_include("vk_startup_draw.h")   // (the start-up screen's own drawing, when that file is in the tree; a fade of 2 is its last picture)
    draw_startup_screen(c, v, startup_fade.load(std::memory_order_relaxed));
    return;
#endif
    const float s = c.s, w = c.w, h = c.h, cx = w * 0.5f, k = s * 0.86f;   // (k: the scale of what is under the logo)
    c.d->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), ui::rgba(14, 10, 6));
    const float lh = 112 * s, top = h * 0.5f - 176 * s;
    for (int i = 40; i >= 1; i--) c.d->AddCircleFilled(ImVec2(cx, top + lh * 0.5f + 70 * s), h * 0.021f * i, ui::rgba(46, 33, 18, 4), 128);
    float aspect = 1;
    ImTextureData* logo = ui::logo(&aspect);
    if (logo) c.d->AddImage(logo->GetTexRef(), ImVec2(cx - lh * aspect * 0.5f, top), ImVec2(cx + lh * aspect * 0.5f, top + lh));
    float y = top + lh + 16 * s;
    ui::spaced(c, c.f.heading, 42 * k, ImVec2(cx, y), ui::rgba(246, 232, 204), "HOOKSHOT", 6 * k);
    ui::flourish(c, cx - 190 * k, cx + 190 * k, y + 66 * k, ui::kAccent);
    ui::text_centre(c, c.f.body, 14.5f * k, ImVec2(cx, y + 78 * k), ui::kDim, v.version.c_str());
    if (!v.title.empty()) {
        y += 120 * k;
        if (!v.step.empty()) {   // ("STEP 2 OF 2" as "Step 2 of 2")
            std::string step = v.step;
            for (size_t i = 1; i < step.size(); i++) step[i] = (char)tolower((unsigned char)step[i]);
            ui::text_centre(c, c.f.body, 15 * k, ImVec2(cx, y), ui::kAccent, step.c_str());
        }
        ui::text_centre(c, c.f.heading, 24 * k, ImVec2(cx, y + 20 * k), ui::kInk, v.title.c_str());
        const float x0 = cx - 270 * k, x1 = cx + 270 * k, by = y + 64 * k;
        float under = by;
        if (v.fraction >= 0) {   // (the bar as a value cell: a dark cell, gold inside, the percentage on it)
            const float bh = 26 * k, in = 4 * k, fx = x0 + in + (x1 - x0 - 2 * in) * (float)std::min(v.fraction, 1.0);
            c.d->AddRectFilled(ImVec2(x0, by), ImVec2(x1, by + bh), ui::kCellDark);
            c.d->AddRect(ImVec2(x0, by), ImVec2(x1, by + bh), ui::kEdge, 0, std::max(1.0f, s));
            if (fx > x0 + in) ui::gold(c, ImVec2(x0 + in, by + in), ImVec2(fx, by + bh - in));
            // (the percentage in the middle of the whole bar, drawn twice: dark ink where the gold is under it, light where the
            // empty bar is, each clipped to its part, so every letter keeps its contrast as the edge passes through it)
            const float split = std::max(fx, x0 + in);
            c.d->PushClipRect(ImVec2(x0, by), ImVec2(split, by + bh), true);
            ui::text_mid_centre(c, c.f.strong, 15 * k, cx, by + bh * 0.5f, ui::kGoldInk, v.pct.c_str());
            c.d->PopClipRect();
            c.d->PushClipRect(ImVec2(split, by), ImVec2(x1, by + bh), true);
            ui::text_mid_centre(c, c.f.strong, 15 * k, cx, by + bh * 0.5f, ui::kInk, v.pct.c_str());
            c.d->PopClipRect();
            under = by + bh;
        }
        const float px = 16.5f * k;
        ui::text(c, c.f.body, px, ImVec2(v.fraction >= 0 ? x0 : cx - ui::size_of(c.f.body, px, v.left.c_str()).x * 0.5f, under + 12 * k),
                 ui::kSoft, v.left.c_str());
        if (!v.right.empty())
            ui::text(c, c.f.body, px, ImVec2(x1 - ui::size_of(c.f.body, px, v.right.c_str()).x, under + 12 * k), ui::kSoft, v.right.c_str());
        if (!v.hint.empty()) ui::text_centre(c, c.f.body, 15.5f * k, ImVec2(cx, by + 92 * k), ui::kDim, v.hint.c_str());
    }
    if (const float f = startup_fade.load(std::memory_order_relaxed); f > 0)   // (the end: to black, the game's first frame next)
        c.d->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), ui::rgba(0, 0, 0, (int)(std::min(f, 1.0f) * 255 + 0.5f)));
}

} // namespace

void startup_fade_to(float f) { startup_fade.store(f, std::memory_order_relaxed); }

void startup_screen(const StartupView* view) {
    if (!view) {
        startup_on.store(false, std::memory_order_release);
        startup_fade.store(0, std::memory_order_relaxed);
        return;
    }
    StartupView v = *view;
    if (v.version.empty()) v.version = version_text(HOOKSHOT_VERSION);   // ("v2.0.5"; "v2.0.5 dev" between releases)
    {
        std::lock_guard lk(startup_mu);
        startup = v;
    }
    startup_on.store(true, std::memory_order_release);
}

int hdr_at_start = 0;
int renodx_at_start = 0, upscale_pre_at_start = 0;
int tex_bc1_at_start = 0;
int tex_detail_at_start = 0;
int replay_threads_at_start = replay_threads::kDefaultRow;
static_assert(Options().replay_threads == replay_threads::kDefaultRow, "the row's default is replay_threads.h's");
bool renodx_chosen() {
    static const char* const e = getenv("RX_VK_RENODX");
    return e && *e ? atoi(e) != 0 : renodx_at_start != 0;
}

void menu_init() {
    static bool done;
    if (done) return;
    done = true;
    for (auto& count : item_counts) count.store(-1, std::memory_order_relaxed);
    bool has_options = false;
    const char* cheats = getenv("HOS_OOT_CHEATS");
    cheats_env = cheats && !strcmp(cheats, "1");
    if (cheats_env) {
        const char* hearts = getenv("HOS_OOT_PROTECT_HEARTS");
        protect_hearts.store(hearts && !strcmp(hearts, "1"));
    }
    if (FILE* f = fopen(ini_path().c_str(), "r")) {
        has_options = true;
        char line[80];
        bool has_resolution = false;
        bool v2 = false;
        int version = 0;
        int old_dynres = -1;   // (the row before Resolution: 0, the full scale, is its 100%; 1 is Auto)
        while (fgets(line, sizeof line, f)) {
            ini_merge::add_line(ini_base, line);   // ((Triforce, handoff §9) the file as the game read it: save's base)
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            for (Row& r : rows) {
                if (!ini_row(r) || strcmp(line, ini_key(r))) continue;
                const int v = atoi(eq + 1);
                *r.value = r.value == &opts.output_res ? output_choice(v) : std::min(std::max(v, r.lo), r.hi);
            }
            has_resolution |= !strcmp(line, "resolution");
            v2 |= !strcmp(line, "options_version");
            if (!strcmp(line, "options_version")) version = atoi(eq + 1);
            if (!strcmp(line, "dynamic_resolution")) old_dynres = atoi(eq + 1);
            if (!strcmp(line, "microphone_device")) {   // (hookshot: the microphone, by its name)
                mic_want = mic_key(eq + 1);
            }
        }
        fclose(f);
        hdr_at_start = opts.hdr;   // (local, RenoDX port: vk_menu.h)
        renodx_at_start = opts.renodx, upscale_pre_at_start = opts.upscale_pre;
        tex_detail_at_start = opts.tex_detail;
        tex_bc1_at_start = opts.tex_bc1;
        replay_threads_at_start = opts.replay_threads;
        // (the saved microphone by its name, not its place; the host told the row's choice. Off or the default: no list, no SDL
        // audio at the start)
        if (!mic_want.empty()) refresh_mics();
        else {
            if (opts.mic >= 2) opts.mic = 1;
            mic_tell_host();
        }
        if (!has_resolution && old_dynres >= 0) opts.resolution = old_dynres ? 0 : 1;
        opts.rumble = rumble_step(opts.rumble);   // (a hand-edited 60: 50, on the arrows' steps)
        // (Game speed is Real time by default now: Per frame played at half speed at 30 fps, the "slow motion" players
        // reported. An options file from before kept the old default whether chosen or not: Real time for it too)
        if (!v2) opts.realtime = 1;
        // (Descriptor reuse is on by default from options_version 4: a file from before wrote its 0 whether chosen or not, as
        // the off default it then was; fewer stutters and a better 1% low in the field, the flash checks clean, 2026-11-10)
        if (version < 4) opts.descriptor_reuse = 1;
        // ((Triforce, 11-16, the owner: "make these game defaults also") From options_version 5 Sampler cache, Fast pipeline
        // link, Lazy write-back and Opaque textures as BC1 are on and NVIDIA Reflex is On + Boost by default: a file from before
        // wrote their old off defaults whether chosen or not. The rows read before the menu (vk_draw.cpp menu_env_on, vk_memory.cpp
        // lazy_decide) take the same rule; Replay helper threads are Automatic and Descriptor reuse on already)
        if (version < 5) {
            opts.sampler_cache = 1, opts.fast_pipeline_link = 1, opts.lazy_write_back = 1, opts.tex_bc1 = 1, opts.reflex = 2;
            tex_bc1_at_start = opts.tex_bc1;
        }
        // ((Triforce, 11-16, the owner: "on by default") From options_version 6 Start next frame early is on by default: a file
        // from before wrote its old off default whether chosen or not. vk_replay.cpp tail_wanted reads it before the menu, by the
        // same rule)
        if (version < 6) opts.tail_early = 1;
        // ((Triforce, 11-17, the owner: "Have frame pacing be on by default for all current installs") From options_version 7
        // Frame pacing is on by default: a file from before wrote its old off default whether chosen or not, so every install
        // turns it on once; one written from 7 on keeps what was chosen)
        if (version < 7) opts.pace = 1;
        // ((Triforce, 11-16, the owner: "remove monitors shape as a option keep 16:9 16:10 21:9 32:9") Before options_version 6
        // the Ultrawide row's 1 was Monitor's shape; now it is 16:10. Such a file's 1 becomes the shape nearest the main
        // monitor's, as host/hos_main.c read it at this start (ultrawide_rows.h)
#ifdef _WIN32
        opts.ultrawide = ultrawide_row_read(opts.ultrawide, version, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
#else
        opts.ultrawide = ultrawide_row_read(opts.ultrawide, version, 0, 0);
#endif
    }
    // An opt-in profile for 4 GB cards: keep existing options authoritative, but make a first run usable on
    // low-VRAM hardware without requiring the menu to render first.
    if (!has_options && getenv("RX_VK_LOW_SPEC") && atoi(getenv("RX_VK_LOW_SPEC")) != 0) {
        apply_preset(1);
        opts.upscale = 0;
        opts.sharpness = 0;
    }
    if (opts.preset > 0) sync_preset_budgets(opts.preset);
    if (const char* e = getenv("RX_VK_FRAMEGEN_MULT")) opts.framegen = std::min(std::max(atoi(e), 2), 8);
    if (const char* e = getenv("RX_VK_PACE")) opts.pace = atoi(e) != 0;
    if (const char* e = getenv("RX_VK_UPSCALE"))
        for (Row& r : rows)
            if (r.value == &opts.upscale) *r.value = std::min(std::max(atoi(e), r.lo), r.hi);
    if (const char* e = getenv("HOS_REALTIME_STEP")) opts.realtime = atoi(e) != 0;
    if (const char* e = getenv("HOS_DYNRES")) opts.resolution = atoi(e) != 0 ? 0 : 1;
    if (const char* e = getenv("HOS_OUTPUT_RES")) opts.output_res = output_choice(atoi(e));
    lang_load();   // (Triforce: the Game language row, from language.txt)
    restart_init();   // (what this run started with: the rows that take a restart differ from it once changed)
}

bool resolution_auto() { return opts.resolution == 0; }
int resolution_scale() {
    const float s = upscaler_rows::percent(opts.resolution) / 100.0f;
    return s > 0 ? (int)(twelfths(s) * 10000.0f / 12.0f + 0.5f) : 0;   // (the twelfth game_size labels)
}

void menu_set_gpu_count(int count) {
    for (Row& r : rows) {
        if (r.value == &opts.gpu_device) {
            r.hi = count;
            if (opts.gpu_device > count) opts.gpu_device = 0;
        }
    }
    restart_init();
    for (int i = 0; i < kRows; i++)   // (the row's value the game started with, the cards counted)
        if (rows[i].value == &opts.gpu_device) restart_state.restart_value(restart_slot[i], opts.gpu_device);
}

bool menu_is_open() { return open_now.load(std::memory_order_relaxed); }
// (the tools off on the Debug tab: flight, the free camera and heart protection put away; -1, heart protection never set, is the
// host's own HOS_OOT_PROTECT_HEARTS)
int menu_heart_protection() {
    const int v = protect_hearts.load(std::memory_order_relaxed);
    return cheats_on() || v < 0 ? v : 0;
}
int menu_player_flight() { return cheats_on() ? player_flight.load(std::memory_order_relaxed) : -1; }
int menu_free_camera() { return cheats_on() ? free_camera.load(std::memory_order_relaxed) : 0; }
/// (Triforce: with the Debug tools turned off meanwhile, what a tool asked and the game's side had not yet taken is dropped,
/// as flight, the free camera and heart protection are put away: the game's side takes it at Link's update, not while paused)
int menu_take_cheat_action() {
    const int action = cheat_action.exchange(0, std::memory_order_relaxed);
    return cheats_on() ? action : 0;
}
int menu_take_share_ask() { return share_ask.exchange(0, std::memory_order_relaxed); }
int menu_take_debug_ask() {
    std::lock_guard lk(names_mu);
    return names_ask.take();
}
int menu_take_amiibo_ask() {
    std::lock_guard lk(names_mu);
    return amiibo_ask.take();
}
int menu_take_mod_cheat() { return mod_cheats.take(); }   // (Triforce: a mod's cheat the player switched, for the host)

// ---- (Triforce) the options' own sounds: the game's interface sounds, after NX2navi's menu sounds. menu_tick says which
// (menu_sound_events.h), the host's sound thread takes them (vk_api.h HOS_OPT_MENU_SOUND) and plays them from the player's
// game files (host/menu_sound.c), at the Audio tab's Menu sounds row's level ----
static_assert(menu_sound::kOpen == HOS_MENU_SOUND_OPEN && menu_sound::kClose == HOS_MENU_SOUND_CLOSE &&
              menu_sound::kCursor == HOS_MENU_SOUND_CURSOR && menu_sound::kDecide == HOS_MENU_SOUND_DECIDE &&
              menu_sound::kCancel == HOS_MENU_SOUND_CANCEL && menu_sound::kTabLeft == HOS_MENU_SOUND_TAB_LEFT &&
              menu_sound::kTabRight == HOS_MENU_SOUND_TAB_RIGHT && HOS_MENU_SOUND_FANFARE == menu_sound::kTabRight + 1 &&
              HOS_MENU_SOUND_SYS_CURSOR == HOS_MENU_SOUND_FANFARE + 1 && HOS_MENU_SOUND_N == HOS_MENU_SOUND_SYS_DECIDE + 1,
              // ((Triforce) the fanfare and the system menu's two: the Mods screen's and entry's own, not the options')
              "menu_sound_events.h's sounds are vk_api.h's HOS_MENU_SOUND_*");
static_assert(menu_sound::kPageAsk == 5, "the restart question is page 5");
static menu_sound::Queue sound_queue;
constexpr int kSoundPageMotion = 7;   // (the Motion page is a tab no strip shows, opened from a row of Controls: a page, to the ear)
/// What the options show now, as far as their sounds go (menu_sound::State): menu_tick takes it before and after its input
static menu_sound::State sound_state() {
    menu_sound::State s;
    s.open = open_now.load();
    if (!s.open) return s;
    s.embedded = embed_open || game_options_shown();
    const bool motion = page == 0 && cur_tab == TAB_MOTION;
    s.page = motion ? kSoundPageMotion : page;
    s.tab = motion ? (int)TAB_CONTROLS : cur_tab;
    auto hashed = [](const std::string& t) {
        int64_t h = 1469598103934665603LL;
        for (const char ch : t) h = (h ^ (unsigned char)ch) * 1099511628211LL;
        return h;
    };
    if (page == 0) {   // (a tab's rows: the row chosen and what it is set to; a debug tool's and a mod's cheat's as they show it)
        const Row& r = rows[tab_row_to_global(cur_tab, tab_selected[cur_tab])];
        s.cursor = tab_selected[cur_tab];
        s.mode = pending ? menu_sound::kModeConfirm : menu_sound::kModeNone;
        if (r.value) s.value = *r.value;
        else if (mod_cheat_slot(r.name) >= 0) s.value = mod_cheats.is_on(mod_cheat_slot(r.name));
        else if (r.cheat >= 0) s.value = hashed(cheat_value(r.cheat));
    } else if (page == 1) {   // (Controls: its own rows' values, a control's bindings)
        const int i = csel - kTop;
        s.cursor = csel * 2 + ccol;
        s.mode = listening ? menu_sound::kModeListen : menu_sound::kModeNone;
        if (i < 0) {
            const Row& r = rows[tab_row_to_global(TAB_PAGE, csel)];
            if (r.value) s.value = *r.value;
        } else if (i < kControls && can_bind()) {
            const int b = controls[i].button;
            s.value = b < HOS_SW_N ? host->binding(HOS_BIND_PAD, b, 0) : 0;
            s.value = (s.value * 4099 + host->binding(HOS_BIND_KEY, b, 0)) * 4099 + host->binding(HOS_BIND_KEY, b, 1);
        }
    } else if (page == 3) {
        s.cursor = teleport_choice;
    } else if (page == 4) {
        s.cursor = item_choice, s.group = inventory_filter;
        s.mode = inventory_amount ? menu_sound::kModeEdit : menu_sound::kModeNone;
        // ((Triforce) what the card shows for the chosen item, the change waiting for the game included: an amount stepped
        // or an item unlocked ticks as a value of a tab's row does)
        s.value = debug_pending::shown(cheat_action.load(std::memory_order_relaxed), item_choice,
                                       item_counts[item_choice].load(std::memory_order_relaxed));
    } else if (page == 5) {
        s.cursor = ask_sel;
    } else if (page == 6) {
        s.cursor = names_page[names_kind].choice, s.group = names_page[names_kind].group;
        s.mode = names_edit ? menu_sound::kModeEdit : menu_sound::kModeNone;
    }
    return s;
}
int menu_take_sound() { return sound_queue.take(prof_now()); }   // (the options' next sound, for the host's sound thread)
void mods_sound(int id) { if (opts.menu_sounds > 0) sound_queue.put(id, prof_now()); }   // ((Triforce) the Mods entry's and screen's own sounds
int menu_mods_selected() { return open_now.load(std::memory_order_relaxed) && (page == kModsRowPage || page == kModsPage); }
int menu_text_wanted() { return mods_text_wanted.load(std::memory_order_relaxed) ? 1 : 0; }   // ((Triforce) the Mods screen's search)
int menu_bug_shot() { return bugshot_state(); }

void menu_note_present_sizes(uint32_t picture_w, uint32_t picture_h, uint32_t window_w, uint32_t window_h) {
    const auto c = [](uint32_t v) { return (uint64_t)std::min(v, 0xffffu); };
    present_sizes.store(c(picture_w) << 48 | c(picture_h) << 32 | c(window_w) << 16 | c(window_h), std::memory_order_relaxed);
}
void menu_note_frame(bool game_frame, int shown) {
    if (shown) shown_frames.fetch_add(shown, std::memory_order_relaxed);
    if (game_frame) game_frames.fetch_add(1, std::memory_order_relaxed);
    if (game_frame && opts.show_fps == 2) {   // (Triforce: the performance overlay's frame times, while it is on Full only)
        std::lock_guard lk(perf_mu);
        perf_stats::note(perf_ring, prof_now());
    }
}

// The Fullscreen row, F10 and Alt+Enter are one setting: the row asks the window's thread (host/display.c), Alt+Enter shows in the row
void fullscreen_tick() {
    static int applied = 0, settle = 0;
    const HWND main_window = app_window();
    if (!main_window) return;
    static const bool env = getenv("HOS_FULLSCREEN") != nullptr;   // (a test's own choice: the row is left alone)
    if (env) return;
    if (opts.fullscreen != applied) {
        applied = opts.fullscreen;
        settle = 60;
        PostMessageA(main_window, WM_APP + 7, (WPARAM)applied, 0);
        return;
    }
    if (settle > 0) { settle--; return; }
    const int now = !(GetWindowLongA(main_window, GWL_STYLE) & WS_CAPTION);
    if (now != applied) {
        opts.fullscreen = applied = now;
        menu_refresh = true;
        save();
    }
}

/// A rate over the half-second that just ended. The first call only starts that interval: the frames since launch,
/// divided by one second, were shown as the rate the moment the counter was turned on.
bool fps_take(int64_t now, int64_t shown, int64_t game, int64_t* next, int64_t* last_shown, int64_t* last_game,
              double* shown_rate, double* game_rate) {
    if (now < *next) return false;
    const bool have = *next != 0;
    if (have) {
        const double secs = (now - *next + 500000000) / 1e9;
        *shown_rate = (shown - *last_shown) / secs;
        *game_rate = (game - *last_game) / secs;
    }
    *last_shown = shown;
    *last_game = game;
    *next = now + 500000000;
    return have;
}

/// (Triforce) The performance overlay's title: "Triforce <this build>, game <the game's version>" in NX2navi's words ("NX2navi
/// %s, game %s"): the build as nx2/version.py stamps it (HOOKSHOT_VERSION: the release's name, "v0.4.0-test41", or git
/// describe's between releases, "v0.4.0-254-g436f9ac-dirty"; as the System tab shows it) without the fingerprint of its
/// sources after it, which would make the panel wider than NX2navi's; the game's 1.1.1, the one build Triforce makes it from
void perf_title(char* out, size_t n) {
    std::string build = HOOKSHOT_VERSION;
    if (const size_t at = build.find(" ("); at != std::string::npos) build.resize(at);
    snprintf(out, n, "Triforce %s, game %s", build.c_str(), kGameVersion);
}
/// (Triforce) A frame rate cap's text as NX2navi's Frame limit says it, "<n> FPS": its first number ("60 (the console's)",
/// "Monitor's rate (144)"); a text with no number as it is ("Monitor's rate", a display that gives none)
const char* perf_rate_text(const char* cap, char* out, size_t n) {
    const char* d = cap;
    while (*d && (*d < '0' || *d > '9')) d++;
    if (*d) snprintf(out, n, "%d FPS", atoi(d));
    else snprintf(out, n, "%s", cap);
    return out;
}
/// (Triforce) The Upscaling setting's value on the panel: the upscaler in effect with its mode ("NVIDIA DLAA (native)", "NVIDIA
/// DLSS, Quality (67%)"); with the game's own the words of its own way ("Game's own (dynamic)", "Game's own, 75% (1440x810)":
/// NX2navi names no scaler of the game's own, its Ocarina leaving the row out). The Upscaler and Render resolution rows
/// keep their own texts
const char* perf_upscaling_text(char* out, size_t n) {
    const char* res = tx_resolution(opts.resolution);
    if (upscaler() == ur::GAME) {
        if (resolution_auto()) snprintf(out, n, "Game's own (dynamic)");
        else snprintf(out, n, "Game's own, %s", res);
    } else if (!strncmp(res, "Native (", 8)) {
        snprintf(out, n, "%s (native)", tx_upscaler(0));
    } else {
        snprintf(out, n, "%s, %s", tx_upscaler(0), res);
    }
    return out;
}
/// (Triforce) Generated frames are shown now: DLSS frame generation's (its multiplier past 1) or frame generation's
/// (vk_framegen.cpp framegen_on: the renderer's own, AMD FSR 3.1's or AMD FSR 4's), as NX2navi shows its FG row by what runs
bool perf_fg_running() { return mkwvk::dlssg::multiplier() > 1 || framegen_on(); }
/// (Triforce) The Frame generation setting's value on the panel: what makes the frames, by name, as NX2navi's (its
/// perf_picture): "NVIDIA DLSS <n>x" while DLSS frame generation runs; while frame generation runs, AMD's FSR 4 library (with
/// the version it runs, NX2navi's fg4_label) where the row is at AMD FSR 4 and the library can make the frame, "AMD FSR 3.1"
/// where FSR 3.1 makes the 2x frame, else the renderer's own, "Triforce's own" with the row's value ("Triforce's own, 4x",
/// "Triforce's own, Auto"); "Off" while nothing makes frames (the row set where none can run too). AMD's library is never
/// asked here (why_known, version: what is known)
const char* perf_framegen_text(char* out, size_t n) {
    if (mkwvk::dlssg::multiplier() > 1) snprintf(out, n, "NVIDIA DLSS %dx", mkwvk::dlssg::multiplier());
    else if (!framegen_on()) snprintf(out, n, "Off");
    else if (opts.framegen == framegen_rows::FSR4 && fsr4fg::offered_known()) {
        const char* ver = fsr4fg::version();
        if (ver[0]) snprintf(out, n, "AMD FSR 4 library (%s)", ver);
        else snprintf(out, n, "AMD FSR 4 library");
    } else if (fsr3fi::frames_wanted()) {
        snprintf(out, n, "AMD FSR 3.1");
    } else {
        snprintf(out, n, "Triforce's own, %s", tx_framegen(opts.framegen));
    }
    return out;
}

/// (Triforce) The performance overlay's texts (NX2navi's full panel, draw_perf), made twice a second while the row is on Full
/// (menu_tick): the machine's latest sample (perf_machine.cpp's thread: nothing here waits for it), the frames' (the ring: the
/// rate, the mean and the longest over the last second, the lows over the last ten, as NX2navi's), the figures per frame
/// since the sample before, and the settings. The counts the figures come from are what the replay keeps anyway, plain counters
/// other threads write: read as they are, a value a moment old is as good here. shown and game: the counter's rates of the
/// last half second, frame generation's frames in shown
static void perf_sample(double shown, double game) {
    PerfView& v = perf_view;
    perf_stats::Summary second, ten;
    {
        std::lock_guard lk(perf_mu);
        if (!v.full) perf_ring = perf_stats::Ring{};   // (Full turned on now: the frames of the time before are not this window's)
        second = perf_stats::summarize(perf_ring, 1000000000ll);
        ten = perf_stats::summarize(perf_ring);
    }
    const bool fresh = !v.full;   // (Full turned on now: the figures per frame start anew too, perf_stats::per_frame)
    v.full = true;
    const auto cell = [](PerfCell& c, const char* fmt, double x) {   // (x < 0: the machine or the game gives none, "-", dimmed)
        c.dim = perf_machine::cell_text(c.t, sizeof c.t, fmt, x);
    };
    const auto empty = [](PerfCell& c) { c.t[0] = 0, c.dim = false; };
    const auto gb = [](double mb) { return mb < 0 ? -1.0 : mb / 1024.0; };
    perf_title(v.title, sizeof v.title);
    const perf_machine::Machine m = perf_machine::latest();
    snprintf(v.gpu_name, sizeof v.gpu_name, "%s", m.gpu_name);
    snprintf(v.cpu_name, sizeof v.cpu_name, "%s", m.cpu_name);
    cell(v.top[0][0], "%.0f%%", m.gpu_pct);
    cell(v.top[0][1], "%.0f \xC2\xB0" "C", m.gpu_temp_c);
    empty(v.top[0][2]);   // ((Triforce, 11-17) NX2navi's watts left out: the card's power is not read, perf_machine.cpp)
    {   // (the card's memory in use by every program and in all; the game's own beside it: what the driver counts for this
        // process on the card's heaps, the budget code's number, vk_vma.cpp. Where the driver does not say, the GPU's clock in
        // its place, as NX2navi's)
        uint64_t used = 0, allowed = 0;
        const bool known = vram_usage(&used, &allowed) && used;
        cell(v.top[1][0], "%.1f GB", gb(m.vram_used_mb));
        cell(v.top[1][1], "/ %.1f GB", gb(m.vram_total_mb));
        if (known) cell(v.top[1][2], "game %.1f GB", (double)used / 1073741824.0);
        else cell(v.top[1][2], "%.0f MHz", m.gpu_clock_mhz);
    }
    cell(v.top[2][0], "%.0f%%", m.cpu_pct);
    cell(v.top[2][1], "%.0f \xC2\xB0" "C", m.cpu_temp_c);
    cell(v.top[2][2], "game %.0f%%", m.cpu_game_pct);
    cell(v.top[3][0], "%.1f GB", gb(m.ram_used_mb));
    cell(v.top[3][1], "/ %.1f GB", gb(m.ram_total_mb));
    cell(v.top[3][2], "game %.1f GB", gb(m.ram_game_mb));
    // (the game's frames: the rate of the last second, the lows of the last ten (from 100 frames on))
    cell(v.top[4][0], "%.0f", second.frames ? second.fps : game);
    cell(v.top[4][1], "1%% low %.0f", ten.low1_fps);
    cell(v.top[4][2], "0.1%% low %.0f", ten.low01_fps);
    // (what is shown while generated frames are: DLSS frame generation's go in at the present, which counts them by its
    // multiplier ((Triforce, 11-16) vk_replay.cpp do_present); the renderer's own, AMD FSR 3.1's and AMD FSR 4's while
    // frame generation runs (vk_framegen.cpp framegen_on: the row set, where it can run, not beside DLSS's). As NX2navi's
    // fg_fps, by what runs and not by the row: a row set where nothing can make frames shows no FG)
    v.fg = perf_fg_running();
    cell(v.top[kPerfFG][0], "%.0f", shown);
    empty(v.top[kPerfFG][1]), empty(v.top[kPerfFG][2]);
    cell(v.top[6][0], "%.1f ms", second.frames ? second.avg_ms : -1.0);
    cell(v.top[6][1], "max %.1f ms", second.frames ? second.max_ms : -1.0);
    empty(v.top[6][2]);
    {   // (per frame since the sample before, over the game's frames since: the replay's time to record the frame (its submits
        // and its present, as the frame trace's replay), the GPU's (its timestamps: none without them), the replay's waits for
        // pipelines and for translations (vk_draw.cpp draw_wait_split), the textures uploaded. The step is perf_stats::per_frame:
        // fresh or the first, every figure "-"; no game frame since, as they were)
        perf_stats::Counts now;
        now.frames = game_frames.load(std::memory_order_relaxed);
        now.prep_ns = prof_ns[PROF_SUBMIT] + prof_ns[PROF_PRESENT], now.gpu_ns = gpu_time_busy_ns();
        int64_t astc = 0, bytes = 0;
        texture_stats(&now.uploads, &astc, &bytes);
        draw_wait_split(&now.pipe_us, &now.shader_us);
        perf_stats::PerFrame f;
        if (perf_stats::per_frame(v.base, now, fresh, &f) != perf_stats::STEP_KEEP) {
            cell(v.frame[0], "%.2f ms", f.prep_ms);
            cell(v.frame[1], "%.2f ms", f.gpu_ms);
            cell(v.frame[2], "%.2f ms", f.pipe_ms);
            cell(v.frame[3], "%.2f ms", f.shader_ms);
            cell(v.frame[4], "%.1f per frame", f.uploads);
        }
    }
    {   // (the settings that shape the picture, kPerfSetting's rows in NX2navi's words and order, from the options' rows here:
        // the window and the game's picture as the last present had them (menu_note_present_sizes); the upscaler in effect with
        // its mode (perf_upscaling_text); the frame rate cap as a rate ("60 FPS"); the output resolution by its lines ("1440p");
        // what makes the frames (perf_framegen_text). An empty one: its row left out.
        // ((Triforce) the Frame limit with who holds it where that is not the game's own wait: "60 FPS (RivaTuner)")
        char t[48];
        const auto set = [&](int i, const char* value) { snprintf(v.setting[i], sizeof v.setting[i], "%s", value ? value : ""); };
        const uint64_t ps = present_sizes.load(std::memory_order_relaxed);
        const unsigned pw = (unsigned)(ps >> 48), ph = (unsigned)(ps >> 32) & 0xffff, ww = (unsigned)(ps >> 16) & 0xffff, wh = (unsigned)ps & 0xffff;
        t[0] = 0;
        if (ww && wh) snprintf(t, sizeof t, "%ux%u", ww, wh);
        set(0, t);
        t[0] = 0;
        if (pw && ph) snprintf(t, sizeof t, "%ux%u", pw, ph);
        set(1, t);
        set(2, perf_upscaling_text(t, sizeof t));
        set(3, tx_aa(opts.aa));
        perf_rate_text(tx_fps_cap(opts.fps_cap), t, sizeof t);   // (and who holds it where the game's own wait does not: vk_pace.cpp)
        const int holder = mkwvk::pace::owner();
        if (holder != mkwvk::pace::GAME) strncat(t, holder == mkwvk::pace::RTSS ? " (RivaTuner)" : " (NVIDIA Reflex)", sizeof t - strlen(t) - 1);
        set(4, t);
        set(5, tx_onoff(opts.vsync));
        set(6, tx_ultrawide(opts.ultrawide));
        unsigned ow = 0, oh = 0;
        t[0] = 0;
        if (sscanf(tx_output_res(opts.output_res), "%ux%u", &ow, &oh) == 2 && oh) snprintf(t, sizeof t, "%up", oh);
        set(7, t);
        set(8, tx_hdr(opts.hdr));
        set(9, perf_framegen_text(t, sizeof t));   // (what makes the frames, by name, as NX2navi's)
    }
    v.have = true;
}

/// (Triforce) The overlay's machine sampler starts (the row went to Full, a GPU chosen): the GPU the game renders on, as
/// Vulkan has it (its name, its LUID, the card's memory: its device-local heaps, NVML's figure over it where it is)
/// ((Triforce: from NX2navi runtime/ui/menu.cpp perf_gpu_once, at 62cc3b13))
void perf_machine_start() {
    perf_machine::Gpu g;
    snprintf(g.name, sizeof g.name, "%s", dev.props.deviceName);
    g.has_luid = gpu_device_luid(g.luid);
    // (NX2navi's: the device-local heaps added up)
    g.vram_total_mb = perf_machine::local_heap_mb(dev.memory.memoryHeaps, dev.memory.memoryHeapCount, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
    perf_machine::start(g, [](const char* line) { logf("vk: the performance overlay: %s", line); });
}

void menu_option_set(int id, int value) {
    if (id >= HOS_OPT_MODS_X0 && id <= HOS_OPT_MODS_PITCH) {   // (Triforce: the Mods entry's list, host/oot_menu.c)
        mods_box[id - HOS_OPT_MODS_X0].store(value, std::memory_order_relaxed);
        return;
    }
    if (id == HOS_OPT_MODS_LIST) {
        mods_list_bits.store(value, std::memory_order_relaxed);
        mods_list_ns.store(prof_now(), std::memory_order_release);
        return;
    }
    if (id == HOS_OPT_MODS_ENTER) {
        mods_enter.store(value, std::memory_order_relaxed);
        return;
    }
    if (id == HOS_OPT_MODS_PRESS) {   // (Triforce: a test's scripted presses while Mods has the cursor, host/nn_hid.c)
        mods_press.fetch_or(value, std::memory_order_relaxed);
        return;
    }
    if (id == HOS_OPT_TEXT_CHAR) {   // ((Triforce) a character typed into the Mods screen's search: host/display.c, the window's thread)
        std::lock_guard<std::mutex> g(mods_typed_mu);
        if (mods_typed.size() < 256) mods_typed.push_back(value);
        return;
    }
    if (id == HOS_OPT_GAME_OPTIONS) {   // (Triforce: the game's own Options screen runs: each frame it does)
        game_options_ns.store(value ? prof_now() : 0, std::memory_order_relaxed);
        return;
    }
    // (Triforce: the cheats of the mods' cheat files, named by the host at the first frame: host/mod_cheats.c. A cheat may not
    // have the name of a row that acts by its name, share_row's)
    if (id == HOS_OPT_MODCHEAT_TEXT) return mod_cheats.text(value);
    if (id == HOS_OPT_MODCHEAT_ADD) {
        static const char* const taken[] = {"Export save", "Export save for a bug report", "Import a save", "Bug capture (F8)"};
        return mod_cheats.add(value, taken, (int)(sizeof taken / sizeof taken[0]));
    }
    if (id == HOS_OPT_MODCHEAT_COUNT) {
        mod_cheats.done(value);
        menu_refresh = true;
        return;
    }
    if (id == HOS_OPT_SHARE_TEXT) {   // (Triforce: the host's line for a share row, a byte a call: host/save_share.c)
        std::lock_guard lk(share_mu);
        if (value > 0 && value < 256 && share_in.size() < 600) share_in += (char)value;
        return;
    }
    if (id == HOS_OPT_SHARE_SAID) {   // (and whose line it is: that row's work is done)
        std::lock_guard lk(share_mu);
        if (value >= HOS_SHARE_EXPORT && value <= HOS_SHARE_NOTICE) share_lines[value] = share_in;
        if (value == HOS_SHARE_NOTICE) share_notice_new.store(true, std::memory_order_release);
        share_in.clear();
        if (value == share_busy_row.load(std::memory_order_relaxed)) share_busy_until.store(0, std::memory_order_relaxed);
        return;
    }
    if (id == HOS_OPT_SHARE_IMPORT) {   // (an imported save waits for the next start, or no longer)
        share_import_waits.store(value != 0, std::memory_order_relaxed);
        return;
    }
    if (id == HOS_OPT_BUG_SHOT) {   // (a bug capture wants the frame shown next as a picture, or no longer: vk_bugshot.cpp)
        bugshot_request(value != 0);
        return;
    }
    if (id == OOT_CHEAT_AGE_ID) {   // (Link's age: the Switch age row shows it)
        if (link_age.exchange(value, std::memory_order_relaxed) != value && page == 0 && cur_tab == TAB_DEBUG) menu_refresh = true;
        return;
    }
    if (id >= OOT_CHEAT_ITEM_BASE && id < OOT_CHEAT_ITEM_BASE + kItems) {   // (the game side's inventory counts)
        if (item_counts[id - OOT_CHEAT_ITEM_BASE].exchange(value, std::memory_order_relaxed) != value && page == 4)
            menu_refresh = true;
        return;
    }
    if (id == HOS_OPT_CHEAT_FLIGHT) {
        if (cheats_on()) {
            int expected = value ? 0 : 1;
            player_flight.compare_exchange_strong(expected, value != 0, std::memory_order_relaxed);
        }
        return;
    }
    if (id == HOS_OPT_DEBUG_TEXT) {   // (Triforce: the game side's line for the name pages, a byte a call: host/hos_main.c)
        std::lock_guard lk(names_mu);
        if (value > 0 && value < 256 && names_in.size() < 600) names_in += (char)value;
        return;
    }
    if (id == HOS_OPT_DEBUG_SAID) {   // (and what the line is: the shown flags' states, or what became of a flag or an event)
        std::lock_guard lk(names_mu);
        if (value == OOT_SAID_FLAGS) names_states.take(names_in.c_str());
        else if (value == OOT_SAID_STATUS) names_status = names_in, names_status_new.store(true, std::memory_order_relaxed);
        names_in.clear();
        if (page == 6) menu_refresh = true;
        return;
    }
    if (id == HOS_OPT_AMIIBO_SAID) {   // (what became of the figure chosen: host/nn_nfp.c)
        if (amiibo_said.exchange(value, std::memory_order_relaxed) != value) amiibo_new.store(true, std::memory_order_relaxed), menu_refresh = true;
        return;
    }
    if (id != 7) return;
    opts.mouse_sens = std::min(std::max(value, 10), 500);   // (the keys work while the menu is closed: it shows it when opened)
    save();
}

/// (hookshot) the mouse on the inventory: a section tab chooses it, a card the item (a click selects it, it does not edit),
/// the page arrows turn the page with the choice, the action button and the amount's halves act; the wheel turns the page
/// shown and leaves the choice where it is; a right click goes back
void inventory_mouse(const MkwVkPointer& ptr, int mx, int my, bool* press, bool& changed) {
    const menu_hit::Hit hit = hits.at((float)mx, (float)my);
    if (ptr.pressed & HOS_MOUSE_LEFT) {
        if (hit.part == menu_hit::kRow && hit.index >= 0 && hit.index <= kItemSections) choose_inventory_filter(hit.index), changed = true;
        else if (hit.part == menu_hit::kListLine && hit.index >= inventory_begin() && hit.index < inventory_end())
            item_choice = hit.index, inventory_amount = false, changed = true;
        else if (hit.part == menu_hit::kButton && hit.index == 0) press[7] = true;
        else if (hit.part == menu_hit::kButton && hit.index == 1 && hit.side) press[hit.side < 0 ? 3 : 4] = true;
        else if (hit.part == menu_hit::kButton && (hit.index == 2 || hit.index == 3))   // (the page arrows)
            press[hit.index == 2 ? 13 : 14] = true, inventory_amount = false;
    }
    if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;
    if (ptr.wheel) {
        const int was = inventory_page;
        inventory_page = std::max(0, inventory_page - ptr.wheel);   // (drawing keeps it to the pages there are)
        changed |= inventory_page != was;
    }
}
/// (hookshot) the mouse on the teleport list: a click on a card selects it (Travel, or confirm, goes), the Travel button
/// goes; the wheel scrolls the cards and leaves the choice where it is; a right click goes back
void teleport_mouse(const MkwVkPointer& ptr, int mx, int my, bool* press, bool& changed) {
    const menu_hit::Hit hit = hits.at((float)mx, (float)my);
    if (ptr.pressed & HOS_MOUSE_LEFT) {
        if (hit.part == menu_hit::kListLine && hit.index >= 0 && hit.index < kTeleports)
            teleport_choice = hit.index, teleport_follow = true, changed = true;
        else if (hit.part == menu_hit::kButton) press[7] = true;
    }
    if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;
    if (ptr.wheel) teleport_wheel += ptr.wheel, teleport_follow = false, changed = true;
}
/// A debug tool's row on the Debug tab (Row::cheat i): Left and Right, and confirm (press[7]: A, Enter, a click). Teleport and
/// Inventory open their pages; back from them comes to the Debug tab, the row still selected
void cheat_press(int i, const bool* press, bool& changed) {
    if (i < 2 && (press[3] || press[4] || press[7])) {   // (Left On, Right Off, as they are drawn; confirm flips)
        const bool was = i == 0 ? protect_hearts.load() == 1 : player_flight.load() >= 0;
        const bool on = press[7] ? !was : press[3];
        if (i == 0) protect_hearts.store(on ? 1 : 0);
        else if (on != was) player_flight.store(on ? 0 : -1);
        changed = true;
    }
    if (i == 4 && (press[3] || press[4])) {
        clock_hour = (clock_hour + (press[4] ? 1 : 23)) % 24;
        changed = true;
    }
    if (i == 5 && (press[3] || press[4])) {
        pouch_item = (pouch_item + kPouchItems + (press[4] ? 1 : -1)) % kPouchItems;
        changed = true;
    }
    if (i == 6 && (press[3] || press[4])) {
        const int step = press[4] ? 1 : -1;
        pouch_delta = std::clamp(pouch_delta + step, -10, 10);
        if (!pouch_delta) pouch_delta = step;
        changed = true;
    }
    if (press[7] && i >= 2 && i <= 4) cheat_action.store(i == 4 ? 100 + clock_hour : i);
    if (press[7] && i == 6) cheat_action.store(200 + pouch_item * 32 + pouch_delta + 10);
    if ((press[3] || press[4] || press[7]) && i == 7) {   // (an On / Off as the first two: Left On, Right Off, confirm flips)
        free_camera.store(press[7] ? !free_camera.load() : press[3] ? 1 : 0);
        changed = true;
    }
    if (press[7] && i == 8) page = 3, teleport_follow = true, changed = true;
    if (press[7] && i == 10) page = 4, choose_inventory_filter(0), changed = true;
    if (press[7] && i >= 11 && i <= 13) names_open(i - 11), changed = true;   // ((Triforce) Game flags, Cutscenes, Scan amiibo)
    if (i == 9 && (press[3] || press[4])) {   // (Kid | Adult: Left Kid, Right Adult, a switch when Link is the other age)
        const int age = link_age.load(std::memory_order_relaxed);
        if (age >= 0 && age != (press[4] ? 1 : 0)) {
            cheat_action.store(999);
            open_now = false;
            changed = true;
        }
    }
    if (press[7] && i == 9) {
        cheat_action.store(999);   // (host/hos_main.c OOT_AGE_ACTION)
        open_now = false;
        changed = true;
    }
}

void mods_open_page();   // ((Triforce) below: the Mods screen opened, its search ended)
/// (Triforce) A step along the tab strip (dir -1 left, 1 right) from the tab or the Mods page shown: the strip's order is
/// ours to System, Mods, Debug when shown; inside the game's Options, past either end is the game's own tab again
void tab_move(int dir) {
    int entry[TAB_COUNT + 2], n = 0;
    for (int t = 0; t < TAB_DEBUG; t++) entry[n++] = t;
    if (debug_shown()) entry[n++] = TAB_DEBUG;
    const int from = page == kModsPage ? kModsTab : cur_tab == TAB_MOTION ? TAB_CONTROLS : cur_tab;
    int at = 0;
    while (at < n - 1 && entry[at] != from) at++;
    if (embed_open && ((dir < 0 && at == 0) || (dir > 0 && at == n - 1))) {
        open_now = false;
        page = 0;
        return;
    }
    const int to = entry[(at + n + dir) % n];
    if (to == kModsTab) {
        page = kModsPage, mods_btn = 0, mods_follow = true;
        cur_tab = TAB_SYSTEM;   // (where Back from it would be)
        mods_open_page();
    } else {
        page = 0, cur_tab = to;
    }
}

/// (Triforce) The Mods page's input: the warning first; the cards (Up and Down, Page Up and Down, Home and End), the file
/// (Left and Right), Install (Confirm); an install's Cancel; its end's Restart Now or Later
/// (Triforce) Back from the Mods screen: to the game's list it was opened from, the cursor on the Mods entry (as the game's own
/// screens give the cursor back to the entry that opened them); the list gone meanwhile, the menu shuts
void mods_back(bool& changed) {
    if (mods_list_fresh()) page = kModsRowPage;
    else open_now = false, page = 0;
    changed = true;
}
/// ((Triforce) 11-17) The pictures' textures of a list the engine emptied (a search's results, a list after a failure): let go
void mods_retire(const std::vector<void*>& textures) {
    for (void* t : textures) texture_retire((ImTextureData*)t);
}
/// ((Triforce) the Mods screen opened: the list asked for (vk_mods.cpp open), a search left behind ended and its text cleared)
void mods_open_page() {
    mods_retire(mods::open());
    mods_query.clear(), mods_query_sent.clear(), mods_kbd = false;
    if (mods_choice < 0) mods_choice = 0;
}
/// ((Triforce) the list made the search of q: the results from the top; fewer than 2 characters is the whole list again,
/// its choice and scroll as they were when the search began
void mods_set_search(const std::string& q, bool& changed) {
    const std::string before = mods::search_text();
    const bool was = !before.empty();
    mods_retire(mods::search(q));
    mods_query_sent = q, changed = true;
    const std::string after = mods::search_text();
    if (after == before) return;   // (the same search, its text trimmed: the list as it is)
    if (!was) mods_browse_choice = mods_choice, mods_browse_px = mods_want_px;
    if (!after.empty()) {
        if (mods_choice >= 0) mods_choice = 0;
        mods_scroll_px = mods_want_px = 0;
    } else if (was) {
        if (mods_choice >= 0) mods_choice = std::max(0, mods_browse_choice);
        mods_scroll_px = mods_want_px = mods_browse_px;
    }
    mods_follow = true, changed = true;
}

void mods_tick(bool* press, bool tab_left, bool tab_right, bool mouse, bool moved, const MkwVkPointer& ptr, int mx, int my, bool& changed) {
    const mods::InstallState inst = mods::install_state();
    const bool busy = inst.job == mods::Job::Downloading || inst.job == mods::Job::Unpacking;
    const bool ended = inst.job == mods::Job::Done || inst.job == mods::Job::Failed || inst.job == mods::Job::Cancelled;
    const bool dialog = !mods::accepted() || busy || ended;
    {   // (the Salesman's big moments: his hello the first time the page shows each run, an install's end; the mask comes off)
        static bool smiled_done = false;
        if (inst.job == mods::Job::Done && !smiled_done) {   // (the game's item-get fanfare, the mask off for it)
            salesman::smile(9), smiled_done = true;
            mods_sound(HOS_MENU_SOUND_FANFARE);
        }
        if (inst.job != mods::Job::Done) smiled_done = false;
        if (!salesman_said_hello && mods::accepted() && salesman::state() == salesman::State::Ready && salesman_tex)
            salesman::smile(2.8f), salesman_said_hello = true;
    }
    static bool was_busy = false;
    if (busy || inst.job == mods::Job::Done) changed = true;   // (the bar moves, the dialog comes: drawn again)
    if (was_busy && !busy) changed = true;
    was_busy = busy;
    // ((Triforce) the text typed since the last tick (host/display.c), the search's; none taken while a dialog is up)
    std::vector<int> typed;
    {
        std::lock_guard<std::mutex> g(mods_typed_mu);
        typed.swap(mods_typed);
    }
    mods_text_wanted.store(mods::available() && !dialog, std::memory_order_relaxed);
    if (dialog) mods_kbd = false;
    bool kbd_click = false;   // (a key clicked: typed, not the keyboard's Enter)
    if (mouse) {
        const menu_hit::Hit hit = hits.at((float)mx, (float)my);
        if (hit.part == menu_hit::kButton && (moved || ptr.pressed) && hit.index != mods_btn) mods_btn = hit.index, changed = true;
        if ((ptr.pressed & HOS_MOUSE_LEFT) && hit.part == menu_hit::kButton) press[7] = true;
        if (!dialog && mods_kbd) {   // ((Triforce) the on-screen keyboard: a key pointed at is chosen, a click types it; a click off it shuts it)
            if (hit.part == menu_hit::kKey && (moved || ptr.pressed)) {
                const int r = hit.index / 10, col = hit.index % 10;
                if (r != mods_kbd_row || col != mods_kbd_col) mods_kbd_row = r, mods_kbd_col = col, changed = true;
            }
            const bool inside = mx >= mods_kbd_box[0] && mx <= mods_kbd_box[2] && my >= mods_kbd_box[1] && my <= mods_kbd_box[3];
            if ((ptr.pressed & HOS_MOUSE_LEFT) && hit.part == menu_hit::kKey) press[7] = true, kbd_click = true;
            else if ((ptr.pressed & HOS_MOUSE_LEFT) && hit.part != menu_hit::kSearch && !inside) mods_kbd = false, changed = true;
        } else if (!dialog) {
            if (hit.part == menu_hit::kListLine && (moved || ptr.pressed) && hit.index != mods_choice) mods_choice = hit.index, mods_follow = true, changed = true;
            if (hit.part == menu_hit::kSearch && (moved || ptr.pressed) && mods_choice != -1) mods_choice = -1, mods_follow = true, changed = true;
            if ((ptr.pressed & HOS_MOUSE_LEFT) && hit.part == menu_hit::kSearch) mods_kbd = true, mods_kbd_row = 1, mods_kbd_col = 0, changed = true;
            if (ptr.pressed & HOS_MOUSE_LEFT) {
                if (hit.part == menu_hit::kTabPrev) tab_left = true;
                else if (hit.part == menu_hit::kTabNext) tab_right = true;
                else if (hit.part == menu_hit::kTab && hit.index == kGameTab) open_now = false, page = 0, changed = true;
                else if (hit.part == menu_hit::kTab && hit.index >= 100 && hit.index < 100 + kModsCats) mods_cat_pick = hit.index - 100;
                else if (hit.part == menu_hit::kTab && hit.index != kModsTab && hit.index >= 0 && hit.index < TAB_COUNT) page = 0, cur_tab = hit.index, changed = true;
            }
            if (ptr.wheel) (mx >= mods_desc_x0 ? mods_desc_wheel : mods_wheel) += ptr.wheel, changed = true;   // (over the details: theirs)
        }
        if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;
    }
    if (!mods::available()) {
        if (press[5]) mods_back(changed);   // (back to the game's list it was opened from)
        return;
    }
    if (!mods::accepted()) {   // (the warning: I understand, or Not now)
        if (press[1] || press[2] || press[3] || press[4]) mods_btn ^= 1, changed = true;
        if (press[7] && mods_btn == 0) {
            mods::accept();
            logf("mods: the warning accepted");
            mods_open_page();
            changed = true;
        } else if (press[5] || press[7]) {   // (Not now: back to the game's list)
            mods_back(changed);
        }
        return;
    }
    if (busy) {   // (only Cancel)
        if (press[5] || press[7]) mods::cancel(), changed = true;
        return;
    }
    if (ended) {
        if (inst.job == mods::Job::Done) {
            if (press[1] || press[2] || press[3] || press[4]) mods_btn ^= 1, changed = true;
            if (press[7] && mods_btn == 0) {   // (Restart Now: as the restart question's, everything saved first)
                logf("mods: restart now, to load %s", inst.mod_name.c_str());
                mods::clear_done();
                save();
                pipeline_cache_save(true);
                memory_trial_orderly_end();
                open_now = false, page = 0, changed = true;
                PostMessageA(app_window(), WM_APP + 9, 0, 0);
            } else if (press[5] || press[7]) {
                mods::clear_done(), mods_btn = 0, changed = true;
            }
        } else if (press[5] || press[7]) {
            mods::clear_done(), mods_btn = 0, changed = true;
        }
        return;
    }
    // ((Triforce) the search: the text typed (the keyboard's characters, Backspace, Ctrl+Backspace), the on-screen keyboard's
    // keys while it is open, and the search made 0.5 s after the typing stops or at once with Confirm)
    const int64_t now = prof_now();
    bool edited = false, search_now = false;
    for (int cp : typed) {
        if (cp == 8) edited |= utf8_pop(mods_query);
        else if (cp == 127) edited |= !mods_query.empty(), mods_query.clear();
        else if (cp >= 0x20 && mods_query.size() < 100) utf8_add(mods_query, cp), edited = true;
    }
    if (edited && !mods_kbd && mods_choice != -1) mods_choice = -1, mods_follow = true;   // (typing: the cursor on the bar)
    if (mods_kbd && !pad_last && !kbd_click && (press[7] || press[5])) {
        if (press[7]) search_now = true;
        mods_kbd = false, changed = true;
        press[7] = press[5] = false;
    }
    if (mods_kbd) {
        const int was_row = mods_kbd_row, was_col = mods_kbd_col;
        if (press[1] || press[2]) {
            mods_kbd_row = (mods_kbd_row + (press[2] ? 1 : 4)) % 5;
            if (mods_kbd_row == 4) mods_kbd_col = kKbdWideAt[kbd_wide(mods_kbd_col)];
        }
        if (press[3] || press[4]) {
            if (mods_kbd_row == 4) mods_kbd_col = kKbdWideAt[(kbd_wide(mods_kbd_col) + (press[4] ? 1 : 3)) % 4];
            else mods_kbd_col = (mods_kbd_col + (press[4] ? 1 : 9)) % 10;
        }
        if (mods_kbd_row != was_row || mods_kbd_col != was_col) changed = true;
        if (press[7]) {
            if (mods_kbd_row < 4) {
                if (mods_query.size() < 100) utf8_add(mods_query, kKbdRows[mods_kbd_row][mods_kbd_col]), edited = true;
            } else {
                const int k = kbd_wide(mods_kbd_col);
                if (k == 0 && mods_query.size() < 100) mods_query += ' ', edited = true;
                else if (k == 1) edited |= utf8_pop(mods_query);
                else if (k == 2) edited |= !mods_query.empty(), mods_query.clear();
                else if (k == 3) search_now = true, mods_kbd = false;
            }
            changed = true;
        }
        if (press[5]) {   // (Back: a character off; with none left the keyboard shut)
            if (!mods_query.empty()) edited |= utf8_pop(mods_query);
            else mods_kbd = false;
            changed = true;
        }
        press[7] = press[5] = false;
        for (int i = 1; i <= 4; i++) press[i] = false;
        for (int i = 13; i <= 16; i++) press[i] = false;
    } else if (press[5]) {   // (Back: a search ended first, its text cleared; then back to the game's list it was opened from)
        if (!mods_query.empty() || !mods::search_text().empty()) {
            mods_query.clear();
            mods_set_search("", changed);
            return;
        }
        mods_back(changed);
        return;
    }
    if (edited) mods_query_edit_ns = now, changed = true;
    const bool pending = mods_query != mods_query_sent;
    if (pending && press[7]) search_now = true, press[7] = false;   // (Confirm with the text changed: its search, not an install)
    // (the on-screen keyboard open: its Search key, or shutting it, searches; its letters one by one do not)
    if (pending && (search_now || (!mods_kbd && now - mods_query_edit_ns > 500000000LL))) mods_set_search(mods_query, changed);
    else if (pending) changed = true;   // (drawn again until it is searched)
    std::string err;
    const mods::Feed feed = mods::feed_state(&err);
    const bool searching = !mods::search_text().empty();
    mods::lock();
    const int n_all = (int)mods::list().size();
    int install = -1;
    bool retry = false, keyboard = false;
    // ((Triforce) 11-17, layout A) L and R turn the category; the cursor moves over its cards (vis), by their places
    if (!mods_kbd && (tab_left || tab_right || mods_cat_pick >= 0)) {
        const int was_cat = mods_cat;
        mods_cat = mods_cat_pick >= 0 ? mods_cat_pick : (mods_cat + kModsCats + (tab_left ? -1 : 1)) % kModsCats;
        mods_cat_pick = -1;
        if (mods_cat != was_cat) {
            mods_sound(tab_left ? HOS_MENU_SOUND_TAB_LEFT : HOS_MENU_SOUND_TAB_RIGHT);
            mods_choice = -2, mods_follow = true, mods_want_px = mods_scroll_px = 0, changed = true;   // (-2: the first card, below)
        }
    }
    std::vector<int> vis;
    for (int i = 0; i < n_all; i++)
        if (mods_in_cat(mods::list()[i], mods_cat)) vis.push_back(i);
    const int n = (int)vis.size();
    if (mods_choice == -2) mods_choice = n ? vis[0] : -1;
    int at = -1;
    for (int k = 0; k < n; k++)
        if (vis[k] == mods_choice) at = k;
    if (mods_choice >= 0 && at < 0) mods_choice = n ? vis[0] : -1, at = n ? 0 : -1;   // (not in this category: its first)
    if (!n && press[7] && feed == mods::Feed::Failed && mods_choice >= 0) retry = true;
    else if (n || mods_choice < 0 || press[1] || press[2] || press[13] || press[14] || press[15] || press[16]) {
        const int was = mods_choice;
        if (press[15]) at = n ? 0 : -1;
        else if (press[16]) at = n - 1;
        else at = std::clamp(at + (press[2] - press[1]) + (press[14] - press[13]) * mods_page_rows, -1, std::max(n - 1, -1));
        mods_choice = at >= 0 ? vis[at] : -1;
        if (mods_choice != was) mods_follow = true, changed = true;
        if (mods_choice >= 0) {
            mods::Mod& m = mods::list()[mods_choice];
            const int files = (int)m.files.size();
            if ((press[3] || press[4]) && files > 1) m.file_choice = (m.file_choice + files + (press[4] ? 1 : -1)) % files, changed = true;
            if (press[7] && m.details_state == 2 && files) install = mods_choice;
        } else if (press[7]) {   // (Confirm on the bar: tried again after a failure; else the on-screen keyboard (a pad), or the results)
            if (!n && feed == mods::Feed::Failed) retry = true;
            else if (pad_last || !n) keyboard = true;
            else mods_choice = 0, mods_follow = true, changed = true;
        }
    }
    mods::unlock();
    if (install >= 0) {
        mods::install(install);
        mods_btn = 0, changed = true;
    } else if (retry) {   // (try again: the search, or the list)
        if (searching) mods_retire(mods::search(mods::search_text()));
        else mods_retire(mods::open());
        changed = true;
    } else if (keyboard) {
        mods_kbd = true, mods_kbd_row = 1, mods_kbd_col = 0, changed = true;
    }
    if (at >= n - 4 && mods::more() && feed == mods::Feed::Ready) mods::load_more();   // (the next page, near the end of the cards shown)
    if (feed == mods::Feed::Loading) changed = true;   // (drawn again as it comes)
    {   // (a picture or the details came in since the last drawing: drawn again, now and then)
        static int64_t last = 0;
        const int64_t now = prof_now();
        if (now - last > 250000000LL) last = now, changed = true;
    }
}

void menu_tick() {
    fullscreen_tick();
    // (Triforce) the Mods entry under the game's list: the host's word that the cursor goes onto it (the game's Down, which the
    // game was not given) or that it was clicked (open at once)
    if (const int enter = mods_enter.exchange(0, std::memory_order_relaxed)) {
        if (!open_now.load() && mods_list_fresh()) {
            open_now = true, embed_open = false, listening = 0, mods_btn = 0, mods_follow = true;
            page = enter == 2 ? kModsPage : kModsRowPage;
            if (enter == 2) mods_open_page();
            auto_pad = pad_last;
            ui::want_glyphs();
            // (its own: before the options' state is taken. (Triforce, 11-17) the system menu's, as the game's list it is under)
            mods_sound(enter == 2 ? HOS_MENU_SOUND_SYS_DECIDE : HOS_MENU_SOUND_SYS_CURSOR);
            logf("menu: the Mods entry %s", enter == 2 ? "clicked" : "has the cursor");
            menu_refresh = true;
        }
    }
    if (!open_now.load() || page != kModsPage) {   // ((Triforce) no text taken off the Mods screen: none kept for it either)
        mods_text_wanted.store(false, std::memory_order_relaxed);
        std::lock_guard<std::mutex> g(mods_typed_mu);
        mods_typed.clear();
    }
    const menu_sound::State sound_before = sound_state();   // ((Triforce) the options' own sounds: told at this tick's end)
    const int64_t sound_share = share_busy_until.load(std::memory_order_relaxed);
    enum { K_N = 17 };
    static bool held[K_N];
    static const int keys[K_N] = {VK_INSERT, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_ESCAPE, VK_F1,   // (F1: for keyboards without Insert)
                                  VK_RETURN, VK_DELETE, VK_BACK,
                                  'Q', 'E', VK_TAB,   // (the tabs: Q and E, Tab and Shift+Tab)
                                  VK_PRIOR, VK_NEXT, VK_HOME, VK_END};   // (the long lists: a page, the ends)
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    const bool mine = pid == GetCurrentProcessId();
    bool press[K_N] = {};
    for (int i = 0; i < K_N; i++) {
        const bool down = mine && (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        const bool queued = host && host->take_key_press && host->take_key_press(keys[i]);
        // Read both keys even if one is disabled, so old presses cannot toggle the menu after a change.
        // (F1 always opens the menu; Menu shortcut's F1 only leaves Insert to an overlay)
        const bool enabled = keys[i] != VK_INSERT || opts.menu_shortcut == 0;
        press[i] = enabled && (host && host->take_key_press ? mine && queued : down && !held[i]);
        held[i] = down;
        if (press[i]) pad_last = false;   // (the keys: their names in the prompts)
    }
    // (hookshot) the gamepads, by position: Home, or Select and Start together, as Insert; the D-pad as the arrows; the
    // buttons the Switch's A and B are bound to as Enter and Esc, as in the game (vk_menu_keys.h; with the Nintendo layout A
    // is the right face button), not while a binding waits for its button
    static uint32_t pad_held = 0;
    const uint32_t raw = mine && host && host->pad_raw ? host->pad_raw() : 0, pad_press = raw & ~pad_held;
    const uint32_t chord = 1u << HOS_PAD_BACK | 1u << HOS_PAD_START;
    const bool chord_now = (raw & chord) == chord && (pad_held & chord) != chord;
    pad_held = raw;
    menu_pad_raw = raw;   // ((Triforce) the Mods screen's description scroll reads the right stick's bits)
    if (pad_press) pad_last = auto_pad = true;   // (a controller: its buttons in the prompts, and Menu size Auto's choice)
    {   // (RX_VK_UI_PROMPTS=pad or keys: the prompts for screenshots of test runs, which have no controller and no focus)
        static const char* force = getenv("RX_VK_UI_PROMPTS");
        if (force) pad_last = !strcmp(force, "pad");
    }
    auto pad = [&](int p) { return listening != 1 && (pad_press >> p & 1) != 0; };
    if (host && host->binding)   // (A and B as bound now: the Controls page or the Layout preset can change them)
        menu_keys::confirm_back(host->binding(HOS_BIND_PAD, HOS_SW_A, 0), host->binding(HOS_BIND_PAD, HOS_SW_B, 0), &pad_confirm,
                                &pad_back);
    if (pad(HOS_PAD_GUIDE) || chord_now) press[0] = true;
    {   // holding an arrow or the D-pad repeats it: after 0.35 s, 12 a second, then 30 a second after 1.5 s held
        static int64_t held_since[5], next_repeat[5];
        const int pads[5] = {0, HOS_PAD_UP, HOS_PAD_DOWN, HOS_PAD_LEFT, HOS_PAD_RIGHT};
        // ((Triforce) 11-17, his "you cannot go up from mods ... with the analog stick": the left stick as the D-pad)
        const int sticks[5] = {0, HOS_PAD_LS_UP, HOS_PAD_LS_DOWN, HOS_PAD_LS_LEFT, HOS_PAD_LS_RIGHT};
        const int64_t t = prof_now();
        for (int i = 1; i <= 4; i++) {
            const bool down = held[i] || (open_now.load() && listening != 1 && ((raw >> pads[i] & 1) || (raw >> sticks[i] & 1)));
            if (!down) { held_since[i] = 0; continue; }
            if (!held_since[i]) { held_since[i] = t, next_repeat[i] = t + 350000000LL; continue; }
            if (t >= next_repeat[i] && open_now.load() && listening == 0) {
                press[i] = true;
                next_repeat[i] = t + (t - held_since[i] > 1500000000LL ? 33000000LL : 83000000LL);
            }
        }
    }
    if (open_now.load() && listening != 1) {
        press[1] |= pad(HOS_PAD_UP) || pad(HOS_PAD_LS_UP), press[2] |= pad(HOS_PAD_DOWN) || pad(HOS_PAD_LS_DOWN);
        press[3] |= pad(HOS_PAD_LEFT) || pad(HOS_PAD_LS_LEFT), press[4] |= pad(HOS_PAD_RIGHT) || pad(HOS_PAD_LS_RIGHT);
        press[5] |= pad(pad_back), press[7] |= pad(pad_confirm);
    }
    int script_tab = 0;   // (a test's TABL or TABR: on the game's own tab too, where Q and E are not ours)
    {   // RX_VK_MENU_SCRIPT (test runs, which never have the focus): timed menu keys, "<seconds>:<INS|UP|DOWN|LEFT|RIGHT|ESC|ENTER|TABL|TABR>,...";
        // and the gamepad's face buttons by position, PADS PADE PADW PADN (bottom, right, left, top), as a controller's press
        static const char* next = getenv("RX_VK_MENU_SCRIPT");
        static const int64_t start = prof_now();
        double at;
        char key[8];
        int n = 0;
        if (next && sscanf(next, "%lf:%7[A-Z]%n", &at, key, &n) == 2 && (prof_now() - start) / 1e9 >= at) {
            static const char* names[] = {"INS", "UP", "DOWN", "LEFT", "RIGHT", "ESC", "", "ENTER", "", "", "TABL", "TABR"};
            for (int i = 0; i < 12; i++) press[i] |= *names[i] && !strcmp(key, names[i]);
            script_tab = !strcmp(key, "TABL") ? -1 : !strcmp(key, "TABR") ? 1 : 0;
            static const char* pads[] = {"PADS", "PADE", "PADW", "PADN"};
            for (int p = 0; p < 4; p++)
                if (!strcmp(key, pads[p]) && open_now.load() && listening != 1) {
                    press[5] |= p == pad_back, press[7] |= p == pad_confirm;
                    pad_last = auto_pad = true;
                }
            next = next[n] == ',' ? next + n + 1 : nullptr;
        }
    }
    {   // (Triforce) F8, a bug capture. The window's key handler asks the host for it itself (host/display.c); where the host has
        // no queue of key presses (Linux) the key is read here, and asked for as the rows ask
        static bool f8_held = false;
        const bool f8_down = mine && !(host && host->take_key_press) && (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (f8_down && !f8_held) share_ask.store(HOS_SHARE_CAPTURE_KEY, std::memory_order_relaxed);
        f8_held = f8_down;
    }
    // (the host's line for the foot of the screen: a capture was saved. One load a tick while there is none)
    if (share_notice_new.load(std::memory_order_relaxed) && share_notice_new.exchange(false, std::memory_order_acq_rel)) {
        std::lock_guard lk(share_mu);
        snprintf(notice_text, sizeof notice_text, "%s", share_lines[HOS_SHARE_NOTICE].c_str());
        notice_until = prof_now() + 4000000000LL;
    }
    if (cur_tab == TAB_DEBUG && !debug_shown()) cur_tab = TAB_DISPLAY;
    {   // (Triforce) presses the host passed on while the Mods entry or its screen has the cursor (a test's scripted ones)
        const int mp = mods_press.exchange(0, std::memory_order_relaxed);
        if (mp && open_now.load() && (page == kModsRowPage || page == kModsPage)) {
            press[1] |= (mp & HOS_MODS_PRESS_UP) != 0, press[2] |= (mp & HOS_MODS_PRESS_DOWN) != 0;
            press[3] |= (mp & HOS_MODS_PRESS_LEFT) != 0, press[4] |= (mp & HOS_MODS_PRESS_RIGHT) != 0;
            press[7] |= (mp & HOS_MODS_PRESS_A) != 0, press[5] |= (mp & HOS_MODS_PRESS_B) != 0;
            script_tab = (mp & HOS_MODS_PRESS_L) ? -1 : (mp & HOS_MODS_PRESS_R) ? 1 : script_tab;
            pad_last = true;
        }
    }
    bool tab_left = press[10] || pad(HOS_PAD_LSHOULDER) || pad(HOS_PAD_LTRIGGER);
    bool tab_right = press[11] || press[12] || pad(HOS_PAD_RSHOULDER) || pad(HOS_PAD_RTRIGGER);
    if (press[12] && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) tab_left = true, tab_right = false;
    // ((Triforce) the upscalers asked ahead once the game has a frame up (menu_ahead), and the Frame generation row set again once
    // they have answered. The game's button icons asked for as the game's own Options screen comes, ahead of our tabs' open:
    // read on a thread of their own, vk_imgui.cpp)
    if (ahead_allowed() && dev.device && !dev.offscreen && game_frames.load(std::memory_order_relaxed) > 0) menu_ahead();
    {
        static bool resynced = false;
        if (!resynced && ahead_state.load(std::memory_order_acquire) == 2) resynced = true, framegen_row_sync();
    }
    // (Triforce) inside the game's own Options screen (see game_options_ns)
    const bool in_game_options = game_options_shown();
    if (in_game_options) ui::want_glyphs();
    if (!in_game_options) {
        embed_since = 0;
        game_tab_read = false;   // ((Triforce) the game's tab's mouse: read anew when its screen comes again)
        if (embed_open) {   // (the game's screen went: ours with it)
            embed_open = false;
            if (open_now.load() && page != 5) open_now = false, page = 0, menu_refresh = true;
        }
    } else {
        if (!embed_since) embed_since = prof_now();
        if (open_now.load()) embed_open = true;   // (opened over it with F1 as well)
    }
    if (in_game_options && !open_now.load()) {
        // The game's own tab: the game has the input. L and R as it has them go to our tabs: the gamepad inputs the Switch's L
        // and R are bound to, and their keys (Q, E and Tab are the game's own keys here: E is its A by default)
        bool go[2] = {script_tab < 0, script_tab > 0};
        static bool key_held[2][2];
        for (int side = 0; side < 2 && host && host->binding; side++) {
            const int sw = side ? HOS_SW_R : HOS_SW_L, p = host->binding(HOS_BIND_PAD, sw, 0);
            if (p >= 0 && p < HOS_PAD_N && (pad_press >> p & 1)) go[side] = true, pad_last = auto_pad = true;
            for (int slot = 0; slot < 2; slot++) {
                const int vk = host->binding(HOS_BIND_KEY, sw, slot);
                const bool down = mine && vk && (GetAsyncKeyState(vk) & 0x8000) != 0;
                if (down && !key_held[side][slot]) go[side] = true, pad_last = false;
                key_held[side][slot] = down;
            }
        }
        // ((Triforce) the mouse on our tabs over the game's header, as on the open menu's: a tab under it lit, a click on one shows
        // it, on the L and R (Q and E) at their ends as L and R. Its presses are the window's for the game's menus too
        // (host/display.c game_menu_press), and host/oot_menu.c leaves the header's to these; elsewhere the game's)
        int clicked_tab = -1;
        if (host && host->pointer) {
            MkwVkPointer gp{};
            const bool on = host->pointer(&gp) != 0 && hits_game_tab;
            if (!game_tab_read) gp.pressed = 0, game_tab_read = true;   // (pressed before the tabs were shown: not theirs)
            int ix = 0, iy = 0;
            menu_hit::to_image(gp.x, gp.y, gp.width, gp.height, (int)dev.swap_extent.width, (int)dev.swap_extent.height, &ix, &iy);
            const menu_hit::Hit hit = on ? hits.at((float)(ix - (int)game_tab_ox), (float)iy) : menu_hit::Hit{};
            hover_tab = hit.part == menu_hit::kTab && hit.index != kGameTab ? hit.index : -1;
            if (on && (gp.pressed & HOS_MOUSE_LEFT)) {
                if (hit.part == menu_hit::kTabPrev) go[0] = true, pad_last = false;
                else if (hit.part == menu_hit::kTabNext) go[1] = true, pad_last = false;
                else if (hover_tab >= 0 && hover_tab < tabs_shown()) clicked_tab = hover_tab, pad_last = false;
            }
        }
        if (go[0] || go[1] || clicked_tab >= 0) {
            const int64_t open0 = prof_now();   // ((Triforce) the open's own work, logged at the first: nothing of it should wait)
            open_now = true, embed_open = true, page = 0, listening = 0;
            cur_tab = clicked_tab >= 0 ? clicked_tab : go[1] ? TAB_DISPLAY : tabs_shown() - 1;
            auto_pad = pad_last;
            ui::want_glyphs();
            opts.pad_layout = detect_pad_layout();
            menu_ahead();   // ((Triforce) whether the Upscaler row can offer DLSS and FSR here: asked off this thread)
            framegen_row_sync();   // (Triforce: whether the Frame generation row can offer AMD FSR 4 here, as known)
            refresh_mics();
            menu_refresh = true;
            static bool told = false;
            if (!told)
                told = true, logf("menu: the options opened from the game's Options in %.1f ms (the upscalers %s)", (prof_now() - open0) / 1e6,
                                  ahead_state.load(std::memory_order_acquire) == 2 ? "answered ahead" : "still being asked, off this thread");
        }
        tab_left = tab_right = false;   // (taken: the first tick of ours does not step on)
    }
    bool changed = menu_refresh;
    menu_refresh = false;
    // (hookshot) the mouse, while the menu is open: the cursor in the window image's pixels (mx, my), tested against the
    // controls as last drawn (hits, vk_menu_hit.h), whether it moved since the last frame, and its clicks and wheel since then
    MkwVkPointer ptr{};
    int mx = -1, my = -1;
    bool mouse = false, moved = false;
    if (open_now.load() && host && host->pointer) {
        mouse = host->pointer(&ptr) != 0;
        int ix = 0, iy = 0;
        menu_hit::to_image(ptr.x, ptr.y, ptr.width, ptr.height, (int)dev.swap_extent.width, (int)dev.swap_extent.height,
                           &ix, &iy);
        if (mouse) mx = ix - (int)menu_ox, my = iy;   // (the menu's column's pixels: draw_menu)
        // ((Triforce) the first read takes no click: one from before the menu opened is not its own. The window keeps the
        // clicks on the game's Options screen for its tabs over the header, host/display.c game_menu_press)
        if (!mouse_read) ptr.pressed = 0;
        static int last_x = -1, last_y = -1;
        moved = mouse && mouse_read && (mx != last_x || my != last_y);   // (not the first read: a cursor resting over the
        last_x = mx, last_y = my, mouse_read = true;                     // menu as it opens leaves the selection alone)
        if (moved) pad_last = false;
        if (!mouse) ptr.pressed = 0, ptr.wheel = 0;
    }
    // (closing with a setting that takes a restart changed: the restart question first, Later already chosen on it)
    auto later = [&] {
        int now[mr::kMax];
        restart_values(now);
        restart_state.choose_later(now);
        open_now = false, page = 0, changed = true;
    };
    // (Triforce) Setup chooses the settings before the game starts, and the menu was hidden for that. 11-10: on by default again,
    // for upstream's new rows that Setup has no place for yet. TRIFORCE_INGAME_MENU=0 (triforce_env.txt) hides it: Insert, F1,
    // Home and Select+Start then open nothing
    // (Triforce, 11-13: off by default now, overlay_allowed above. While the options are open, from inside the game's Options,
    // the keys still close them)
    static const bool menu_allowed = overlay_allowed();
    if ((press[0] || press[6]) && open_now.load() && page == 5) {
        later();
    } else if ((press[0] || press[6]) && open_now.load() && restart_ask()) {
        page = 5, ask_sel = 0, listening = 0, changed = true;
    } else if ((press[0] || press[6]) && (menu_allowed || open_now.load())) {
        open_now = !open_now.load(), changed = true;
        page = 0, listening = 0;
        // (the layout row shows the controls as they are: host/nn_hid.c keeps them, the ini's pad_ lines, and a saved
        // layout is not applied over them at start)
        if (open_now.load()) auto_pad = pad_last;   // (Menu size Auto: by what opened the menu)
        if (open_now.load()) ui::want_glyphs();   // (Triforce: the game's button icons, as it was served them this run)
        if (open_now.load()) opts.pad_layout = detect_pad_layout();
        if (open_now.load()) menu_ahead();   // (whether the Upscaler row can offer DLSS and FSR here: asked once, (Triforce) off this thread)
        if (open_now.load()) framegen_row_sync();   // (Triforce: and the Frame generation row AMD FSR 4, as known)
        if (open_now.load()) refresh_mics();   // (the microphones as they are now)
    }
    if (open_now.load() && page == 5) {   // (hookshot) the restart question: Later (the default) or Restart now
        if (mouse) {   // (a button under the cursor chosen, a click on it pressed, a right click is Later; no list: the wheel does nothing)
            const menu_hit::Hit hit = hits.at((float)mx, (float)my);
            if (hit.part == menu_hit::kButton && (moved || ptr.pressed) && hit.index != ask_sel) ask_sel = hit.index, changed = true;
            if ((ptr.pressed & HOS_MOUSE_LEFT) && hit.part == menu_hit::kButton) press[7] = true;
            if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;
        }
        if (press[1] || press[2] || press[3] || press[4] || tab_left || tab_right) ask_sel ^= 1, changed = true;
        if (press[5] || (press[7] && ask_sel == 0)) {
            later();
        } else if (press[7]) {   // (Restart now: everything saved, then the window closes as the player would close it)
            logf("menu: restart now, to apply %s", restart_list(", ").c_str());
            save();
            pipeline_cache_save(true);
            memory_trial_orderly_end();   // (an exit on purpose: no verdict on the memory trial)
            open_now = false, page = 0, changed = true;
            PostMessageA(app_window(), WM_APP + 9, 0, 0);
        }
    } else if (open_now.load() && page == 1) {   // (hookshot) the Controls page
        if (!can_bind()) page = 0;
        else if (listening) {
            if (press[5] || prof_now() > listen_end) listening = 0, changed = true;   // (Esc, or nothing for a while)
            else if (listening == 1) {
                if (!listen_armed) listen_armed = (raw & ~(1u << HOS_PAD_GUIDE) & ((1u << HOS_PAD_N) - 1)) == 0;   // (not the stick's menu bits)
                else if (const uint32_t p = pad_press & ~(1u << HOS_PAD_GUIDE)) {
                    int bit = 0;
                    while (!(p >> bit & 1)) bit++;
                    host->bind(HOS_BIND_PAD, controls[csel - kTop].button, 0, bit);
                    listening = 0, changed = true;
                    opts.pad_layout = detect_pad_layout();
                    save();
                }
            } else {
                int down = 0;
                for (int k : bindable_keys())
                    if (mine && (GetAsyncKeyState(k) & 0x8000)) {
                        down = k;
                        break;
                    }
                if (!listen_armed) listen_armed = !down;
                else if (down) {
                    host->bind(HOS_BIND_KEY, controls[csel - kTop].button, 0, down);
                    host->bind(HOS_BIND_KEY, controls[csel - kTop].button, 1, 0);
                    listening = 0, changed = true;
                    save();
                }
            }
        } else {
            if (mouse) {   // (the mouse: the row under it selected, a click on a binding listens for its new input, on
                           // Reset resets; the wheel scrolls the page, a right click goes back)
                const menu_hit::Hit hit = hits.at((float)mx, (float)my);
                const bool option = hit.part == menu_hit::kCtlOption;
                const bool on = option || hit.part == menu_hit::kCtlRow || hit.part == menu_hit::kCtlCell;
                const int at = option ? hit.index : kTop + hit.index;   // (the page's row: its own rows first)
                if (on && (moved || ptr.pressed)) {
                    if (at != csel) csel = at, changed = true;
                    if (hit.part == menu_hit::kCtlCell && !keys_only(hit.index) && ccol != hit.side) ccol = hit.side, changed = true;
                }
                if (ptr.pressed & HOS_MOUSE_LEFT) {
                    if (option) press[hit.side < 0 ? 3 : 4] = true;   // (a value's halves: Left and Right; the name: Right)
                    else if (hit.part == menu_hit::kCtlCell || (hit.part == menu_hit::kCtlRow && hit.index == kControls))
                        press[7] = true;   // (as Enter)
                }
                if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;
                if (ptr.wheel) wheel_rows[kControlsSlot] += ptr.wheel, changed = true;   // (scrolls; the selection stays)
            }
            if (press[5]) page = 0, changed = true;
            const int n = kTop + kControls + 1, i = csel - kTop;   // (i: the control's, or reset's)
            if (press[1]) csel = (csel + n - 1) % n, changed = true;
            if (press[2]) csel = (csel + 1) % n, changed = true;
            if (i < 0 && (press[3] || press[4] || press[7])) {   // (the page's own rows: Enter as Right)
                Row& r = rows[tab_row_to_global(TAB_PAGE, csel)];   // (Custom is only what the bindings are: set_row skips it)
                set_row(r, *r.value + (press[3] ? -r.step : r.step), press[3] ? -1 : 1);
                changed = true;
            } else if (i >= 0) {
                if ((press[3] || press[4]) && i < kControls && !keys_only(i)) ccol ^= 1, changed = true;
                if (keys_only(i)) ccol = 1;   // (the stick's and the camera's directions: keys only)
                if (press[7]) {
                    if (i == kControls) {
                        host->bind(HOS_BIND_PAD, -1, 0, 0);
                        opts.pad_layout = 0;
                        save();
                    }
                    else listening = ccol ? 2 : 1, listen_armed = false, listen_end = prof_now() + 6000000000LL;
                    changed = true;
                }
                if ((press[8] || press[9]) && i < kControls) {
                    if (ccol == 0) {
                        host->bind(HOS_BIND_PAD, controls[i].button, 0, -1);
                        opts.pad_layout = detect_pad_layout();
                    }
                    else for (int k = 0; k < 2; k++) host->bind(HOS_BIND_KEY, controls[i].button, k, 0);
                    changed = true;
                    save();
                }
            }
        }
    } else if (open_now.load() && page == 4) {   // the inventory
        const int chosen = item_choice;   // (the page follows the choice when it moves; the wheel's page stays otherwise)
        if (mouse) inventory_mouse(ptr, mx, my, press, changed);
        if (press[5] && inventory_amount) inventory_amount = false, changed = true;   // (back: out of the amount first)
        else if (press[5]) page = 0, changed = true;   // (to the Debug tab, Inventory still selected)
        else if (tab_left || tab_right) {   // (another section, from editing an amount too: that ends)
            const int filters = kItemSections + 1;
            choose_inventory_filter((inventory_filter + filters + (tab_left ? -1 : 1)) % filters);
            changed = true;
        } else if (inventory_amount) {
            if (press[7]) inventory_amount = false, changed = true;
            else if (press[3] || press[4]) {   // ((Triforce) added to the change still waiting, debug_pending.h: each press counts)
                const int count = item_counts[item_choice].load(std::memory_order_relaxed);
                int was = cheat_action.load(std::memory_order_relaxed);
                while (count >= 0 && !cheat_action.compare_exchange_weak(was, debug_pending::amount_step(was, item_choice, count, press[4] ? 1 : -1),
                                                                         std::memory_order_relaxed)) {}
                changed = true;
            }
        } else {
            // (the cards as drawn, inventory_columns to a row: the arrows move, Page Up and Down a page, Home and End the ends)
            const int begin = inventory_begin(), end = inventory_end();
            const int step = (press[4] - press[3]) + (press[2] - press[1]) * inventory_columns + (press[14] - press[13]) * inventory_capacity;
            if (step || press[15] || press[16]) {
                item_choice = press[15] ? begin : press[16] ? end - 1 : std::clamp(item_choice + step, begin, end - 1);
                inventory_reveal();
                changed = true;
            } else if (press[7] && item_counts[item_choice].load(std::memory_order_relaxed) >= 0) {
                if (item_kinds[item_choice]) inventory_amount = true, changed = true;   // (an amount: Left and Right change it)
                else {   // (unlock or relock; (Triforce) a second press before the game took the first takes it back)
                    int was = cheat_action.load(std::memory_order_relaxed);
                    while (!cheat_action.compare_exchange_weak(was, debug_pending::toggle(was, item_choice), std::memory_order_relaxed)) {}
                    changed = true;
                }
            }
            if (item_choice != chosen) inventory_reveal();   // (a click on a card: its page)
        }
    } else if (open_now.load() && page == 6) {   // (Triforce) a name page: Game flags, Cutscenes, Scan amiibo
        if (mouse) names_mouse(ptr, mx, my, press, changed);
        names_press(press, tab_left, tab_right, changed);
    } else if (open_now.load() && page == 3) {   // the teleport list
        if (mouse) teleport_mouse(ptr, mx, my, press, changed);
        if (press[5]) page = 0, changed = true;   // (to the Debug tab, Teleport still selected)
        else if (press[7]) {
            cheat_action.store(1000 + teleport_choice);
            open_now = false, page = 0;   // (back to the game: the map change starts with an event)
            changed = true;
        } else if (tab_left || tab_right) {   // (the first map of the previous or next section)
            const int section = (teleport_section_of(teleport_choice) + kTeleportSections + (tab_left ? -1 : 1)) % kTeleportSections;
            teleport_choice = teleport_sections[section].first, teleport_follow = true;
            changed = true;
        } else {
            // (the cards as drawn, each section's in rows under its heading: Up and Down a row, the column kept; Left and Right the
            // previous and next card; Page Up and Down the rows that fit; Home and End the ends)
            const int rows = (press[2] - press[1]) + (press[14] - press[13]) * teleport_page_rows;
            const int was = teleport_choice;
            if (press[15]) teleport_choice = 0;
            else if (press[16]) teleport_choice = kTeleports - 1;
            else teleport_choice = std::clamp(teleport_step_rows(teleport_choice, rows) + (press[4] - press[3]), 0, kTeleports - 1);
            if (teleport_choice != was) teleport_follow = true, changed = true;
        }
    } else if (open_now.load() && page == kModsRowPage) {   // (Triforce) the cursor on the Mods entry under the game's list
        if (mouse && (ptr.pressed & HOS_MOUSE_LEFT) && hits.at((float)mx, (float)my).part == menu_hit::kButton) press[7] = true;
        if (!mods_list_fresh()) {   // (the game's list went: its screen closed under it)
            open_now = false, page = 0, changed = true;
        } else if (press[7]) {   // (A: Mods opens)
            page = kModsPage, mods_btn = 0, mods_follow = true, changed = true;
            mods_open_page();
        } else if (press[1] || press[5] || press[0] || press[6] || tab_left || tab_right) {   // (Up or Back: the game's cursor again)
            open_now = false, page = 0, changed = true;
        }
    } else if (open_now.load() && page == kModsPage) {   // (Triforce) the Mods page
        mods_tick(press, tab_left, tab_right, mouse, moved, ptr, mx, my, changed);
    } else if (open_now.load()) {
        if (mouse) {   // (the mouse: see the top of this file)
            const menu_hit::Hit hit = hits.at((float)mx, (float)my);
            const int ht = hit.part == menu_hit::kTab ? (hit.index == kGameTab ? -1 : hit.index) : hit.part == menu_hit::kTabPrev ? TAB_COUNT
                         : hit.part == menu_hit::kTabNext ? TAB_COUNT + 1 : -1;
            if (ht != hover_tab) hover_tab = ht, changed = true;
            int& sel = tab_selected[cur_tab];
            const bool on_row = hit.part == menu_hit::kRow || hit.part == menu_hit::kSlider;
            if (drag_row >= 0) {   // (a slider held: it follows the cursor's x until let go)
                Row& d = rows[tab_row_to_global(cur_tab, drag_row)];
                if (!(ptr.held & HOS_MOUSE_LEFT) || !d.value) {
                    drag_row = -1;
                    save();
                } else {
                    const int permille = hits.drag((float)mx, drag_row);
                    const int v = permille < 0 ? *d.value : menu_hit::slider_value(permille, d.lo, d.hi, d.step);
                    if (v != *d.value) set_row(d, v, v > *d.value ? 1 : -1, false), changed = true;
                }
            } else if (on_row && (moved || ptr.pressed) && hit.index != sel) {
                sel = hit.index, changed = true;
            }
            if ((ptr.pressed & HOS_MOUSE_LEFT) && drag_row < 0) {
                if (hit.part == menu_hit::kTabPrev) tab_left = true;
                else if (hit.part == menu_hit::kTabNext) tab_right = true;
                else if (hit.part == menu_hit::kTab && hit.index == kGameTab) open_now = false, changed = true;   // (the game's own tab)
                else if (hit.part == menu_hit::kTab && hit.index == kModsTab) {   // (Triforce: the Mods page)
                    page = kModsPage, mods_btn = 0, mods_follow = true, cur_tab = TAB_SYSTEM, changed = true;
                    mods_open_page();
                }
                else if (hit.part == menu_hit::kTab && hit.index != cur_tab && hit.index < tabs_shown()) cur_tab = hit.index, changed = true;
                else if (hit.part == menu_hit::kSlider) {
                    Row& d = rows[tab_row_to_global(cur_tab, hit.index)];
                    drag_row = hit.index;
                    const int v = menu_hit::slider_value(hit.permille, d.lo, d.hi, d.step);
                    if (v != *d.value) set_row(d, v, v > *d.value ? 1 : -1, false);
                    changed = true;
                } else if (hit.part == menu_hit::kRow) {
                    Row& d = rows[tab_row_to_global(cur_tab, hit.index)];
                    if (!d.value && d.cheat >= 0 && (cheat_chooses(d.cheat) || age_shown(d.cheat) || d.cheat <= 1 || d.cheat == 7) &&
                        hit.side != 0)
                        press[hit.side < 0 ? 3 : 4] = true;   // (a tool's choice: its halves as Left and Right)
                    else if (!d.value && hit.halves && mod_cheat_slot(d.name) >= 0)
                        press[hit.side < 0 ? 3 : 4] = true;   // ((Triforce) a mod's cheat: On, its left half, as Left; Off as Right)
                    else if (!d.value) press[7] = true;   // (a page, a tool: as Enter)
                    else if (hit.halves && d.hi - d.lo == d.step) {   // ((Triforce) a toggle's half: that value, as the game's own
                        // toggles are picked; the left half is the high value (On), the right the low. The one it has: nothing)
                        if (*d.value != (hit.side < 0 ? d.hi : d.lo)) press[4] = true;
                    }
                    else if (d.hi - d.lo == d.step || hit.side >= 0) press[4] = true;   // (a toggle flips; the right half, or the name, as Right)
                    else press[3] = true;                                               // (the left half as Left)
                }
            }
            if (ptr.pressed & HOS_MOUSE_RIGHT) press[5] = true;   // (as Esc)
            // (the wheel scrolls the rows, wherever the cursor is; it never changes a value, the selection or the tab: the
            // values change by a click on their arrows or halves, a slider's drag, Left and Right, A and Enter)
            if (ptr.wheel && drag_row < 0) wheel_rows[cur_tab] += ptr.wheel, changed = true;
        }
        if (press[5] && cur_tab == TAB_MOTION) cur_tab = TAB_CONTROLS, press[5] = false, changed = true;   // (back)
        if (press[5] && pending) pending = nullptr, changed = true;   // (a Restart or Quit waiting for its second press: cancelled)
        else if (press[5] && cur_tab == TAB_MOTION) cur_tab = TAB_CONTROLS, press[5] = false, changed = true;   // (back)
        else if (press[5] && restart_ask()) page = 5, ask_sel = 0, changed = true;   // (the restart question first)
        else if (press[5]) open_now = false;
        // (the tabs: LB and RB, Q and E, or Tab; from the Motion page, as from Controls)
        if ((tab_left || tab_right) && cur_tab == TAB_MOTION) cur_tab = TAB_CONTROLS;
        if (tab_left || tab_right) {   // (inside the game's Options: past our first or last tab is the game's own again; Mods
                                       // after System: tab_move)
            tab_move(tab_left ? -1 : 1);
            changed = true;
        }
        const int tab_count = tab_row_count(cur_tab);
        int& selected = tab_selected[cur_tab];
        if (selected >= tab_count) selected = tab_count - 1;   // (a row gone since: the debug tools with them turned off)
        auto heading_at = [&](int k) { return rows[tab_row_to_global(cur_tab, k)].heading; };   // (stepped over)
        if (heading_at(selected)) selected = (selected + 1) % tab_count;
        if (cur_tab == TAB_DEBUG && (press[1] || press[2])) {   // (the Debug tab: in its layout's order, debug_rows)
            selected = debug_step(selected, press[2] ? 1 : -1);
            changed = true;
        } else {
            if (press[1]) {
                do selected = (selected + tab_count - 1) % tab_count;
                while (heading_at(selected));
                changed = true;
            }
            if (press[2]) {
                do selected = (selected + 1) % tab_count;
                while (heading_at(selected));
                changed = true;
            }
        }
        Row& r = rows[tab_row_to_global(cur_tab, selected)];
        const bool confirm_selected = &r == &rows[kQuitRow] || &r == &rows[kRestartRow];
        if (pending && (pending != &r || !mine)) pending = nullptr, changed = true;
        if (press[7] && confirm_selected) {
            if (mine && !press[5]) {
                if (pending != &r) {
                    pending = &r, changed = true;
                } else {   // (as Restart now: saved first, as the window's own close does not force the pipeline cache)
                    save();
                    pipeline_cache_save(true);
                    memory_trial_orderly_end();
                    pending = nullptr;
                    if (&r == &rows[kRestartRow]) {   // (the restart question's Restart now: the window closes, then the game starts again)
                        logf("menu: restart");
                        open_now = false, changed = true;
                        if (const HWND window = app_window()) PostMessageA(window, WM_APP + 9, 0, 0);
                    } else if (const HWND window = app_window()) {
                        PostMessageA(window, WM_CLOSE, 0, 0);
                    }
                }
            }
        } else if (r.cheat >= 0) cheat_press(r.cheat, press, changed);   // (a debug tool: on the Debug tab itself)
        // (Triforce: a mod's cheat: Left On, Right Off, confirm flips; the host takes the change, menu_take_mod_cheat. Before the
        // rows that act by their name or open a page: a cheat's name is the file's)
        else if (mod_cheat_slot(r.name) >= 0) changed |= mod_cheats.press(mod_cheat_slot(r.name), press[3], press[4], press[7]);
        else if (press[7] && share_row(r)) share_press(share_row(r)), press[7] = false, changed = true;   // (Triforce: the host does it)
        else if (press[7] && is_motion_row(r)) cur_tab = TAB_MOTION, tab_selected[TAB_MOTION] = 0, press[7] = false, changed = true;
        else if (press[7] && !r.value && can_bind()) page = 1, csel = 0, ccol = 0, changed = true;
        if ((press[3] || press[4] || press[7]) && r.value) {   // (Enter, or A, as Right)
            if (press[7]) press[4] = true;
            set_row(r, *r.value + (press[4] ? r.step : -r.step), press[4] ? 1 : -1);
            changed = true;
        }
        const int64_t now = prof_now();
        if (changed) {   // (a new row or value: its name from the start, after a rest)
            marquee_pos = 0, marquee_next = now + kMarqueeRest;
        } else if (marquee_max > 0 && now >= marquee_next) {
            if (marquee_pos >= marquee_max) marquee_pos = 0, marquee_next = now + kMarqueeRest;   // (back to the start)
            else if (++marquee_pos >= marquee_max) marquee_next = now + kMarqueeRest;            // (rest at the end)
            else marquee_next = now + kMarqueeStep;
            changed = true;
        }
    }
    {   // (Triforce) the options' own sounds, the game's interface sounds: what this tick's input did to the options names one
        // sound at most (menu_sound_events.h sound_of), which waits for the host's sound thread (menu_take_sound). None is
        // asked for while the Menu sounds row is at 0. A save row and the Motion row take their press for themselves
        // (press[7] is false again by now): the save rows' is told from the time their answer is waited for, which a press
        // moves on (the host's answer meanwhile sets it back to 0: no press)
        menu_sound::Input in;
        in.toggle = press[0] || press[6], in.back = press[5];
        in.confirm = press[7] || share_busy_until.load(std::memory_order_relaxed) > sound_share;
        in.tab_left = tab_left, in.tab_right = tab_right, in.left = press[3], in.right = press[4];
        in.move = press[1] || press[2] || press[13] || press[14] || press[15] || press[16], in.clear = press[8] || press[9];
        in.click = (ptr.pressed & HOS_MOUSE_LEFT) != 0, in.drag = drag_row >= 0;
        // ((Triforce) 11-17, his "it makes a different noise from the regular menu sounds") The Mods entry under the game's list
        // sounds as that list does, not as the options: A the system menu's "decide" (the Mods screen opens), the cursor going
        // back to the game's list its cursor. Coming onto the entry is told where the host's word comes (menu_tick's top)
        const menu_sound::State after = sound_state();
        const bool row_before = sound_before.open && sound_before.page == kModsRowPage;
        if (row_before) {
            if (after.open && after.page == kModsPage) mods_sound(HOS_MENU_SOUND_SYS_DECIDE);
            else if (!after.open) mods_sound(HOS_MENU_SOUND_SYS_CURSOR);
        } else if (sound_before.open || !after.open || after.page != kModsRowPage) {
            if (opts.menu_sounds > 0) sound_queue.put(menu_sound::sound_of(sound_before, sound_state(), in), prof_now());
        }
    }
    names_tick();   // ((Triforce) the name pages: their lists, the flags asked for, what the game's side said)
    // (Triforce) the Salesman's thread draws only while the Mods page shows him
    salesman::show(open_now.load() && page == kModsPage && mods::available() && mods::accepted(), salesman_w, salesman_h);
    if (open_now.load() && page == kModsPage) changed = true;   // (he moves: drawn again)
    {   // (hookshot) the window lets the mouse go while the menu is open, and takes it back after (host/display.c)
        static bool told = false;
        const bool now_open = open_now.load();
        if (!now_open) {
            if (drag_row >= 0) save();   // (a slider let go off the window: kept now)
            if (!in_game_options) hover_tab = -1;   // ((Triforce) on the game's own tab, the tabs' own: above)
            drag_row = -1, mouse_read = false;
            pending = nullptr;
        }
        if (now_open != told && app_window()) {
            told = now_open;
            PostMessageA(app_window(), WM_APP + 8, (WPARAM)now_open, 0);
        }
    }
    if (opts.show_fps != 2) perf_view.full = false, perf_view.have = false;   // (Triforce: not on Full: the next Full starts its frame times anew)
    {   // (Triforce: the machine's numbers, sampled on a thread of their own while the row is on Full and a GPU is chosen:
        // perf_machine.cpp. Here only its start and its stop)
        static bool machine = false;
        const bool want = opts.show_fps == 2 && dev.props.deviceName[0];
        if (want != machine) {
            machine = want;
            if (want) perf_machine_start();
            else perf_machine::stop();
        }
    }
    if (opts.show_fps) {
        static int64_t next = 0, last_game = 0, last_shown = 0;
        const int64_t now = prof_now();
        double shown = 0, game = 0;
        if (fps_take(now, shown_frames.load(), game_frames.load(), &next, &last_shown, &last_game, &shown, &game)) {
            snprintf(fps_text, sizeof fps_text, "%.0f", shown);
            // ((Triforce, 11-16) the game's own rate beside it with DLSS frame generation too, while it generates)
            if (opts.framegen >= 1 || mkwvk::dlssg::multiplier() > 1) snprintf(fps_sub, sizeof fps_sub, "%.0f", game);
            else fps_sub[0] = 0;
            if (opts.show_fps == 2) perf_sample(shown, game);   // (Triforce: the Full overlay's lines)
        }
    }
}

void menu_record(VkCommandBuffer cmd, uint32_t index, uint32_t w, uint32_t h) {
    const bool start = startup_on.load(std::memory_order_acquire);
    if (!start) bugshot_record(cmd, index, w, h);   // (Triforce: a bug capture's picture of this frame, before anything is drawn over it)
    const bool fps = opts.show_fps && !start;   // (no game frames yet: not "0 FPS" over the start-up screen)
    const bool open = open_now.load(std::memory_order_relaxed);
    const bool note = !open && !start && notice_text[0] && prof_now() < notice_until;
    // (Triforce: the game's own Options screen is up and none of our tabs: its header's tabs alone)
    const int64_t game_seen = game_options_ns.load(std::memory_order_relaxed);
    const bool game_tab = !open && !start && embed_allowed() && game_seen && prof_now() - game_seen < 250000000LL && embed_since;
    const bool mods_row = !open && !start && mods_list_fresh();   // ((Triforce) the Mods entry under the game's list)
    if (!start && !fps && !open && !note && !game_tab && !mods_row) {   // (nothing shows: no ImGui frame, no render pass)
        // (but a new window size or Menu size since the last bake: one frame with nothing drawn that bakes the menu's glyphs
        // and measures its page for the new size, so the next open has no hitch whatever was resized while nothing showed)
        if (prebake_needed(w, h) && ui::begin(w, h)) {
            ui::Ctx c = menu_ctx(w, h);
            prebake(c);
            ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (Dear ImGui records into cmd directly, its backend through its own pointers: what this thread recorded through vk_work.h reaches cmd first)
            ui::record(cmd, index, w, h);
            return;
        }
        ui::idle();
        return;
    }
    if (!ui::begin(w, h)) return;
    {   // (the easing's step: the time since the last drawing, a quarter of the way in 1/60 s)
        static int64_t last = 0;
        const int64_t now = prof_now();
        ease_step = last ? std::min(1.0f, (float)((now - last) / 1e9) * 15.0f) : 1.0f;
        last = now;
        ease_live = true;
    }
    ui::Ctx c = menu_ctx(w, h);
    prebake(c);   // (the menu's glyphs at this size, before it opens: no hitch then)
    if (start) {   // (under the menu, in the overlay's own fonts)
        ui::Ctx sc = ui_ctx(w, h);
        draw_startup(sc);
    }
    // ((Triforce) Full: NX2navi's panel alone, no counter beside it (its perf_draw), and under the options or the game tab's
    // header while they show (its BringWindowToDisplayBack): the open menu covers it)
    const bool full = fps && opts.show_fps == 2;
    if (open) draw_menu(c, full);
    else {
        if (full) draw_perf(c);
        if (game_tab) draw_game_tab(c);
        else if (mods_row) draw_mods_row_shut(c);
    }
    if (fps && !full) draw_fps(c, (open && page != kModsRowPage) || game_tab);   // ((Triforce) the Mods entry: the counter stays where the game list has it)
    if (note) draw_notice(c);
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (Dear ImGui records into cmd directly, its backend through its own pointers: what this thread recorded through vk_work.h reaches cmd first)
    ui::record(cmd, index, w, h);
}

void menu_record_image(VkCommandBuffer cmd, VkImage image, VkImageView view, uint32_t w, uint32_t h) {
    const bool open = open_now.load(std::memory_order_relaxed);
    const bool fps = opts.show_fps && !startup_on.load(std::memory_order_acquire);
    const bool note = !open && notice_text[0] && prof_now() < notice_until;
    const bool game_tab = !open && game_options_shown() && embed_since;
    const bool mods_row = !open && mods_list_fresh();
    if (!open && !fps && !note && !game_tab && !mods_row) return;
    if (!ui::begin(w, h)) return;
    // ((Triforce) a still: with no window's frames stepping the easing (menu_record), the lists stand where the selection asks.
    // Before, a headless shot, and tests/ui_preview.cpp's pictures, showed every list at its top whatever was selected)
    if (!ease_live) ease_step = 1.0f;
    ui::Ctx c = menu_ctx(w, h);
    const bool full = fps && opts.show_fps == 2;   // ((Triforce) as menu_record: the panel alone, under the menu)
    if (open) draw_menu(c, full);
    else {
        if (full) draw_perf(c);
        if (game_tab) draw_game_tab(c);
        else if (mods_row) draw_mods_row_shut(c);
    }
    if (fps && !full) draw_fps(c, (open && page != kModsRowPage) || game_tab);   // ((Triforce) the Mods entry: the counter stays where the game list has it)
    if (note) draw_notice(c);
    ::mkwvk::rec::boundary(::mkwvk::rec::B_DIRECT);   // (Dear ImGui records into cmd directly, its backend through its own pointers: what this thread recorded through vk_work.h reaches cmd first)
    ui::record_to(cmd, image, view, w, h);
}

} // namespace mkwvk
