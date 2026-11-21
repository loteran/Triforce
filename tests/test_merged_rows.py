"""The Insert menu's options_version 7 (Triforce): the rows grouped, the rows that chose one thing twice merged into one
(gpu/vk/merged_rows.h), the rows whose job is done otherwise gone. An options file from before must play the same.

Checked without a game or a GPU:
- merged_rows.h's views and their writes back, over every saved value they stand for: HDR (hdr, renodx_hdr), Frame
  generation (frame_generation, dlss_frame_generation), Frame pacing (frame_pacing, steady_frame_rate), Low latency
  (nvidia_reflex or amd_anti-lag_2 by the card's maker), and its migration (handheld_profile, stick_dead_zone);
- the menu's own row table, ini_key, ini_row and menu_init's reading of the file, compiled with stand-ins and fed a copy of
  an owner's options file of version 6 (an RX 7900 XT: HDR Native with RenoDX, AMD FSR 4 at Performance, Upscale before
  bloom, 21:9, Frame pacing on): every value lands where it did, the merged rows show what ran (Native + RenoDX, Frame
  pacing Even, Low latency Off), no line of the file is lost unless merged_rows.h drops it, no two rows save one key, the
  readers before the menu still find their keys, and the file the menu then writes reads back to the same values.

python -I -B tests/test_merged_rows.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"

# (a copy of an owner's triforce_options.ini as version 6 wrote it, without the controls' pad_ and key_ lines)
OWNER = """fullscreen=1
output_res=1
ultrawide=2
ultrawide_hud=0
ultrawide_menus=1
hdr=2
hdr_paper_white=200
hdr_peak_brightness=1000
renodx_hdr=1
renodx_tonemapper=1
renodx_gamma_correction=0
renodx_midgray=1
renodx_match_tone_slope=1
renodx_tone_slope=50
renodx_hue_clip=50
renodx_ui_brightness=200
renodx_color_space=2
renodx_exposure=100
renodx_highlights=50
renodx_shadows=50
renodx_contrast=50
renodx_saturation=50
renodx_highlight_saturation=50
renodx_blowout=0
renodx_flare=0
renodx_hue_correction=100
renodx_color_grading=100
renodx_bloom=50
frame_rate_cap=5
steady_frame_rate=0
reflex_frame_limit=0
vsync=0
fps_counter=2
gpu_device=0
quality_preset=0
dlss=4
dlss_model=0
upscale_before_bloom=1
resolution=4
anti-aliasing=0
upscaling=0
sharpness=0
texture_filtering=0
texture_detail=0
frame_generation=0
dlss_frame_generation=0
frame_pacing=1
nvidia_reflex=2
amd_anti-lag_2=0
handheld_profile=0
robust_buffers=0
game_speed=1
text_speed=0
master_volume=100
menu_sounds=80
audio_output=0
microphone=1
discord_status=1
controller_in_background=0
mouse_camera=0
mouse_sensitivity=100
invert_mouse_vertical=0
camera_speed=100
invert_camera_x=0
invert_camera_y=0
sprint_button=0
sprint_mode=0
stick_dead_zone=0
left_stick_dead_zone=-1
right_stick_dead_zone=-1
rumble_strength=100
vibration_curve=100
trigger_vibration=0
walk_key=0
walk_scale=51
motion_controls=1
motion_sensitivity_x=100
motion_sensitivity_y=100
invert_motion_x=0
invert_motion_y=0
motion_deadzone=1
layout_preset=0
button_prompts=0
menu_shortcut=0
debug_tools=0
descriptor_reuse=1
fast_pipeline_link=1
sampler_cache=1
lazy_write-back=1
replay_helper_threads=8
tail_early=1
opaque_textures_as_bc1=1
options_version=6
"""

# (the keys read before the menu, by the code that needs them: host/*.c hos_ini_option and line reads, vk_dlssg.cpp,
# vk_main.cpp, vk_draw.cpp menu_env_on, vk_memory.cpp, vk_replay.cpp, vk_texture.cpp)
READ_BEFORE = ["output_res", "frame_rate_cap", "hdr", "renodx_hdr", "steady_frame_rate", "ultrawide", "ultrawide_hud", "sprint_button",
               "sprint_mode", "master_volume", "camera_speed", "audio_output", "button_prompts", "dlss_frame_generation",
               "robust_buffers", "descriptor_reuse", "sampler_cache", "fast_pipeline_link", "lazy_write-back", "tail_early"]

MERGED = r"""
#include <cstdio>
#include <initializer_list>
#include "@VK@/merged_rows.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
namespace mr = mkwvk::merged_rows;
int main() {
    /* HDR: every saved pair has its view, and the view written back is the same pair where RenoDX runs */
    for (int hdr = 0; hdr <= 2; hdr++)
        for (int renodx = 0; renodx <= 1; renodx++) {
            const int row = mr::hdr_row(hdr, renodx);
            CHECK(row == (hdr == 2 && renodx ? mr::HDR_RENODX : hdr));
            int h = -1, r = -1;
            mr::hdr_set(row, h, r);
            CHECK(h == hdr && (r == renodx || hdr != 2));   /* (RenoDX off with Off and Expanded: it ran only with Native) */
        }
    for (int row = 0; row <= 3; row++) {
        int h = 0, r = 0;
        mr::hdr_set(row, h, r);
        CHECK(mr::hdr_row(h, r) == row);
    }
    /* Frame generation: the renderer's own and AMD FSR 4's, then NVIDIA DLSS 2x to 4x; one sets the other off */
    for (int fg = 0; fg <= 9; fg++)
        for (int dlssg = 0; dlssg <= 3; dlssg++)
            CHECK(mr::framegen_row(fg, dlssg) == (dlssg ? mr::FG_DLSS + dlssg - 1 : fg));
    for (int row = 0; row <= mr::FG_DLSS_LAST; row++) {
        int fg = 5, dlssg = 2;
        mr::framegen_set(row, fg, dlssg);
        CHECK(mr::framegen_row(fg, dlssg) == row && (row < mr::FG_DLSS ? dlssg == 0 && fg == row : fg == 0 && dlssg == row - 9));
    }
    int list[16];
    CHECK(mr::framegen_choices(list, false, false) == 9 && list[8] == 8);
    CHECK(mr::framegen_choices(list, true, false) == 10 && list[9] == 9);
    CHECK(mr::framegen_choices(list, true, true) == 13 && list[10] == mr::FG_DLSS && list[12] == mr::FG_DLSS_LAST);
    CHECK(mr::framegen_choices(list, false, true) == 12 && list[9] == mr::FG_DLSS);
    /* Frame pacing: Off, Even, Steady (any rate), Steady (refresh steps) */
    for (int pace = 0; pace <= 1; pace++)
        for (int steady = 0; steady <= 2; steady++)
            CHECK(mr::pacing_row(pace, steady) == (steady ? 1 + steady : pace));
    for (int row = 0; row <= 3; row++) {
        int pace = -1, steady = -1;
        mr::pacing_set(row, pace, steady);
        CHECK(mr::pacing_row(pace, steady) == row && pace == (row != 0) && steady == (row >= 2 ? row - 1 : 0));
    }
    /* Low latency: Reflex's three on an NVIDIA card, Anti-Lag 2's two on an AMD one, the other vendor's value left alone */
    for (int v = 0; v <= 2; v++) {
        int reflex = 1, antilag = 1;
        mr::latency_set(v, mr::LATENCY_NVIDIA, reflex, antilag);
        CHECK(reflex == v && antilag == 1 && mr::latency_row(mr::LATENCY_NVIDIA, reflex, antilag) == v);
        reflex = 2, antilag = 0;
        mr::latency_set(v, mr::LATENCY_AMD, reflex, antilag);
        CHECK(reflex == 2 && antilag == (v > 0) && mr::latency_row(mr::LATENCY_AMD, reflex, antilag) == (v > 0));
    }
    CHECK(mr::latency_hi(mr::LATENCY_NVIDIA) == 2 && mr::latency_hi(mr::LATENCY_AMD) == 1 && mr::latency_hi(mr::LATENCY_NONE) == 0);
    CHECK(mr::latency_row(mr::LATENCY_NONE, 2, 1) == 0);
    /* step_in: wraps, and a value that is not a choice steps to its neighbours */
    const int some[] = {0, 1, 2, 10, 11};
    CHECK(mr::step_in(some, 5, 0, 1) == 1 && mr::step_in(some, 5, 11, 1) == 0 && mr::step_in(some, 5, 0, -1) == 11);
    CHECK(mr::step_in(some, 5, 9, 1) == 10 && mr::step_in(some, 5, 9, -1) == 2 && mr::step_in(some, 5, 12, 1) == 0);
    /* the migration: the handheld profile, the shared dead zone; nothing for a file of version 8 (v0.5.1's 7 still had the
       rows: it is moved too) */
    for (int version : {0, 5, 6, 7, 8}) {
        mr::Before7 old;
        old.handheld = 1, old.stick_shared = 3;
        int out = 1, l = -1, r = 20;
        mr::migrate(version, old, out, l, r);
        CHECK(version >= 8 ? out == 1 && l == -1 : out == mr::kHandheldOutput && l == 15);
        CHECK(r == 20);
        old = mr::Before7{};
        out = 2, l = -1, r = -1;
        mr::migrate(version, old, out, l, r);
        CHECK(out == 2 && l == -1 && r == -1);
    }
    std::puts("merged rows: views, writes back, choices and the migration: all checks passed");
    return 0;
}
"""

LOAD = r"""
#define NOMINMAX
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif
#include "@VK@/vk_api.h"
#include "@VK@/upscaler_rows.h"
#include "@VK@/aa_rows.h"
#include "@VK@/framegen_rows.h"
#include "@VK@/merged_rows.h"
#include "@VK@/ultrawide_rows.h"
#include "@VK@/replay_threads.h"
#include "@VK@/ini_merge.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
namespace mkwvk {
@OPTIONS@
Options opts;
namespace ur = upscaler_rows;
namespace mr7 = merged_rows;
@TABS@
@ROW@
@OUT@
@STUBS@
constexpr int kMaxMics = 16;
constexpr char kModCheatHelp[] = "";
struct { char names[96][8]; } mod_cheats;
@MOD_ROWS@
@MERGED@
@TABLE@
constexpr int kRows = sizeof rows / sizeof rows[0];
@INI_KEY@
@INI_ROW@
/* (stand-ins for what menu_init's reading touches besides the rows) */
std::string path;
std::string ini_path() { return path; }
ini_merge::Map ini_base;
std::string mic_want;
std::string mic_key(const char* name) { return name; }
void refresh_mics() {}
void mic_tell_host() {}
int rumble_step(int v) { return (std::min(std::max(v, 0), 300) + 12) / 25 * 25; }
int hdr_at_start, renodx_at_start, upscale_pre_at_start, tex_detail_at_start, tex_bc1_at_start, replay_threads_at_start;
bool has_options = false;
void load() {
@LOAD@
}
/* (the card's maker for Low latency: an AMD card, as the owner's) */
mr7::Vendor latency_vendor() { return mr7::LATENCY_AMD; }
@SYNC@
@RESTART@
}  // namespace mkwvk
using namespace mkwvk;

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    path = argv[1];
    load();
    merged_sync();
    /* what ran before runs: HDR Native with RenoDX, AMD FSR 4 at its Performance mode (resolution 4, 50%), Upscale before
       bloom, 21:9 with the menus' sides blurred, a cap of 240, VSync off, the FPS counter Full, the frames paced evenly, no
       frame generation; AMD Anti-Lag 2 off */
    CHECK(opts.hdr == 2 && opts.renodx == 1 && merged.hdr == mr7::HDR_RENODX);
    CHECK(opts.dlss == ur::FSR4 && ur::engine(opts.dlss) == ur::FSR4 && !ur::full_scale(opts.dlss));
    CHECK(opts.resolution == 4 && ur::percent(opts.resolution) == 50 && !std::strcmp(ur::dlss_mode_name(opts.resolution), "Performance"));
    CHECK(opts.upscale_pre == 1 && opts.ultrawide == 2 && opts.wide_hud == 0 && opts.wide_menus == 1 && opts.fullscreen == 1);
    CHECK(opts.fps_cap == 5 && opts.vsync == 0 && opts.show_fps == 2 && opts.output_res == 1);
    CHECK(opts.pace == 1 && opts.steady_fps == 0 && merged.pacing == mr7::PACE_EVEN);
    CHECK(opts.framegen == 0 && opts.dlssg == 0 && merged.framegen == 0);
    CHECK(opts.antilag == 0 && opts.reflex == 2 && merged.latency == 0);
    CHECK(opts.hdr_white == 200 && opts.hdr_peak == 1000 && opts.rdx_tonemap == 1 && opts.rdx_midgray == 1 && opts.rdx_space == 2 &&
          opts.rdx_ui_white == 200 && opts.rdx_hue == 100 && opts.rdx_grade == 100 && opts.rdx_match_slope == 1);
    CHECK(opts.lstick_dead_zone == -1 && opts.rstick_dead_zone == -1 && opts.stick_dead_zone == 0);
    CHECK(opts.realtime == 1 && opts.replay_threads == 8 && opts.tail_early == 1 && opts.tex_bc1 == 1 && opts.walk_scale == 51);
    /* (the Reflex frame limit: off, its old default and the file's 0, until it has been played: vk_menu.h reflex_limit) */
    CHECK(opts.upscale == 0 && opts.reflex_limit == 0 && opts.vram_mb == 0 && opts.mirror_mb == 0);
    CHECK(hdr_at_start == 2 && renodx_at_start == 1 && upscale_pre_at_start == 1);
    /* no two rows save one key, and the merged rows save none of their own */
    std::set<std::string> keys;
    for (const Row& r : rows) {
        if (!ini_row(r)) continue;
        CHECK(keys.insert(ini_key(r)).second);
        CHECK(!is_merged(r.value));
    }
    /* each line the file had is read by a row or dropped by merged_rows.h, its job done otherwise */
    FILE* f = std::fopen(argv[1], "r");
    char line[128];
    int lines = 0;
    while (f && std::fgets(line, sizeof line, f)) {
        char* eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        bool dropped = false;
        for (const char* d : mr7::kDropped) dropped |= !std::strcmp(line, d);
        CHECK(keys.count(line) || dropped || !std::strcmp(line, "options_version"));
        lines++;
    }
    if (f) std::fclose(f);
    CHECK(lines > 90);
    /* the readers before the menu find their keys in the file the menu writes */
    for (int i = 3; i < argc; i++) CHECK(keys.count(argv[i]));
    /* the file the menu then writes (every saved row by its key, options_version 7) reads back to the same values */
    const Options before = opts;
    const Merged shown = merged;
    {
        FILE* w = std::fopen(argv[2], "w");
        for (const Row& r : rows)
            if (ini_row(r)) std::fprintf(w, "%s=%d\n", ini_key(r), *r.value);
        std::fprintf(w, "options_version=%d\n", mr7::kVersion);
        std::fclose(w);
    }
    opts = Options{};
    path = argv[2];
    load();
    merged_sync();
    CHECK(!std::memcmp(&before, &opts, sizeof opts));
    CHECK(shown.hdr == merged.hdr && shown.framegen == merged.framegen && shown.pacing == merged.pacing && shown.latency == merged.latency);
    /* an older file with the rows that went: the handheld profile as the Output resolution's, the shared dead zone each
       stick's own, the other lines without effect */
    {
        FILE* w = std::fopen(argv[2], "w");
        std::fprintf(w, "output_res=2\nhandheld_profile=1\nstick_dead_zone=2\nright_stick_dead_zone=30\nquality_preset=3\nupscaling=2\n"
                        "reflex_frame_limit=0\noptions_version=6\n");
        std::fclose(w);
    }
    opts = Options{};
    load();
    CHECK(opts.output_res == mr7::kHandheldOutput && opts.lstick_dead_zone == 10 && opts.rstick_dead_zone == 30);
    CHECK(opts.upscale == 0 && opts.reflex_limit == 0 && opts.vram_mb == 0);
    /* the HDR row takes a restart for Native + RenoDX alone (the scene's RGBA16F target and RenoDX's shaders are chosen at
       the start); Off, Expanded and Native change at once, as the HDR row did */
    const Row* hdr = nullptr;
    for (const Row& r : rows)
        if (r.value == &merged.hdr) hdr = &r;
    CHECK(hdr && hdr->restart);
    for (int row = 0; row <= 3; row++) {
        mr7::hdr_set(row, opts.hdr, opts.renodx);
        merged_sync();
        CHECK(merged.hdr == row && restart_value_of(*hdr) == (row == mr7::HDR_RENODX ? 1 : 0));
    }
    opts.hdr = 2, opts.renodx = 1;   /* (RenoDX chosen with HDR Off or Expanded runs nothing: no restart for it) */
    merged_sync();
    CHECK(restart_value_of(*hdr) == 1);
    opts.hdr = 1;
    CHECK(restart_value_of(*hdr) == 0);
    std::puts("merged rows: an owner's file of version 6 plays the same, and what is written reads back the same");
    return 0;
}
"""


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def block(text, at):
    start = text.index("{", at)
    depth = 0
    for i in range(start, len(text)):
        depth += text[i] == "{"
        depth -= text[i] == "}"
        if depth == 0:
            return text[at:i + 1]
    raise SystemExit("FAIL: no end for the block at %d" % at)


def definition(text, head):
    found = re.search(re.escape(head) + r"\([^;{]*\)\s*\{", text)
    check(found, "no definition of " + head)
    return block(text, found.start())


def load_fixture():
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")
    header = (VK / "vk_menu.h").read_text(encoding="utf-8")
    table = menu[menu.index("Row rows[] = {"):]
    table = table[:table.index("\n};\n") + 4]
    # (the rows' words and lines: stand-ins, this is about what is saved)
    names = sorted(set(re.findall(r"\b((?:tx|hn)_[a-z0-9_]+)\b", table)))
    stubs = "\n".join("const char* %s(int%s) { return nullptr; }" % (n, ", int" if n.startswith("hn_") else "") for n in names)
    mod_rows = menu[menu.index("#define MOD_CHEAT_ROW(i)"):menu.index("static_assert(mod_cheat_rows::kSlots")]
    merged = menu[menu.index("struct Merged {"):menu.index("Row rows[] = {")]
    out = menu[menu.index("struct Out {"):menu.index("int step_output_choice(int id, int dir) {")]
    init = definition(menu, "void menu_init")
    reading = block(init, init.index('if (FILE* f = fopen(ini_path().c_str(), "r")) {'))
    check("merged_rows::migrate(version, before7, opts.output_res, opts.lstick_dead_zone, opts.rstick_dead_zone);" in reading,
          "menu_init's reading of the file does not migrate an older one")
    sync = definition(menu, "void merged_sync")
    fixture = LOAD
    for mark, text in (("@OPTIONS@", re.search(r"struct Options \{.*?\n\};", header, re.S).group(0)),
                       ("@TABS@", re.search(r"enum MenuTab \{.*?\};", menu, re.S).group(0)),
                       ("@ROW@", re.search(r"struct Row \{.*?\n\};", menu, re.S).group(0)),
                       ("@OUT@", out), ("@STUBS@", stubs), ("@MOD_ROWS@", mod_rows), ("@MERGED@", merged), ("@TABLE@", table),
                       ("@INI_KEY@", definition(menu, "const char* ini_key")), ("@INI_ROW@", definition(menu, "bool ini_row")),
                       ("@LOAD@", reading), ("@SYNC@", sync), ("@RESTART@", definition(menu, "int restart_value_of"))):
        fixture = fixture.replace(mark, text)
    return fixture.replace("@VK@", VK.as_posix())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")
    check('out += "options_version=8\\n";' in definition(menu, "void save"), "save does not write options_version=8")
    check("for (const char* dropped : merged_rows::kDropped) mine |= key == dropped;" in definition(menu, "void save"),
          "save writes back the lines of the rows that went")
    with tempfile.TemporaryDirectory(prefix="merged-rows-") as tmp:
        tmp = Path(tmp)
        src, exe = tmp / "merged.cpp", tmp / "merged.exe"
        src.write_text(MERGED.replace("@VK@", VK.as_posix()), encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        src, exe = tmp / "load.cpp", tmp / "load.exe"
        src.write_text(load_fixture(), encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-variable",
                        "-Wno-unused-parameter", "-I", str(ROOT / "gpu" / "render"), str(src), "-o", str(exe)], check=True)
        owner = tmp / "triforce_options.ini"
        owner.write_text(OWNER, encoding="ascii")
        subprocess.run([str(exe), str(owner), str(tmp / "written.ini")] + READ_BEFORE, check=True)
    print("PASS: options_version 7: the merged rows, the migration, and an owner's file of version 6 as it played")


if __name__ == "__main__":
    main()
