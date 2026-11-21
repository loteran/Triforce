"""Exercise the host and menu's docked output sizes without a game dump."""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()

    host = (ROOT / "host/nn_applet.c").read_text(encoding="utf-8")
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    fixture = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

using DWORD = unsigned long;
constexpr int MAX_PATH = 260;
DWORD hos_game_path_a(char *path, int size) { return snprintf(path, size, "./hookshot.exe"); }
void hos_log(const char *, ...) {}
volatile uint64_t hos_wide_out = 0;   // (host/hos_main.c: the widescreen picture, 0 = none; nn_applet.c reads it)
int hos_scene_rgba16f(void) { return 0; }   // (host/nn_vi.c: the scene's colour target, the game's own here; tests/test_rt_pool_minimum.py has the RGBA16F cases)
struct { int handheld, output_res; } opts;
namespace mkwvk {
int up_now = 0;   // (the Upscaler shown: 0 the game's own, 1 DLSS, 2 DLAA)
int upscaler() { return up_now; }
}
#include "gpu/vk/upscaler_rows.h"
#include "gpu/vk/merged_rows.h"   // ((Triforce, options_version 7) the Output resolution's handheld profile, 7)
using namespace mkwvk;
namespace ur = upscaler_rows;
''' + host[host.index("int hos_ini_option(const char *key, int otherwise);"):host.index("HLE(_ZN2nn2oe16GetOperationModeEv)")] + menu[
        menu.index("struct Out {"):menu.index("/// The Render resolution: the value chosen")] + menu[
        menu.index("const char* tx_output_res("):menu.index("const char* tx_speed(")] + r'''
int main(int argc, char **argv) {
    assert(argc == 5);
    const int mode = atoi(argv[1]);
    const unsigned w = atoi(argv[2]), h = atoi(argv[3]);
    assert(output_mode() == mode);
    assert(hos_output_size(0) == w && hos_output_size(1) == h);
    assert(hos_rt_pool_size(305ull << 20) == ((mode == 2 ? 687ull : mode == 4 ? 610ull : mode == 5 || mode == 6 ? 900ull : 512ull) << 20));   // (512 MB at least: nn_applet.c)
    // ((Triforce, options_version 7) 7 is 1536x864, the handheld profile, the smallest: first in the row)
    assert(kLastOutput == 7 && merged_rows::kHandheldOutput == 7);
    assert(output_choice(-1) == 0 && output_choice(4) == 4 && output_choice(5) == 5 && output_choice(6) == 6 && output_choice(7) == 7 &&
           output_choice(8) == 0);
    assert(step_output_choice(7, -1) == 7 && step_output_choice(7, 1) == 3 && step_output_choice(3, -1) == 7);
    assert(strcmp(tx_output_res(7), "1536x864 (handheld)") == 0);
    assert(step_output_choice(3, 1) == 0 && step_output_choice(0, -1) == 3);
    assert(step_output_choice(0, 1) == 1 && step_output_choice(1, -1) == 0);
    assert(step_output_choice(1, 1) == 2 && step_output_choice(2, -1) == 1);
    assert(step_output_choice(2, 1) == 4);
    assert(step_output_choice(4, -1) == 2 && step_output_choice(4, 1) == 6);   // ((Triforce) 5120x2160 between 5120x1440 and 5120x2880)
    assert(step_output_choice(6, -1) == 4 && step_output_choice(6, 1) == 5);
    assert(step_output_choice(5, -1) == 6 && step_output_choice(5, 1) == 5);
    opts.output_res = mode;
    opts.handheld = handheld();
    assert(strcmp(tx_output_res(mode), argv[4]) == 0);
    if (opts.handheld) opts.output_res = 7;   // (the menu's row: the handheld profile, as menu_init reads it, merged_rows.h migrate)
    unsigned scene_w, scene_h;
    game_size(1.0f, scene_w, scene_h);
    assert(scene_w == (opts.handheld ? 1536 : mode ? w : 2048));
    assert(scene_h == (opts.handheld ? 864 : mode ? h : 1152));
    if (mode == 3) assert(strcmp(tx_resolution(2), "75% (1440x810)") == 0);
    if (mode == 3) {   // (DLSS at 1080p: its modes' render sizes, each a twelfth of the output, exact; DLAA the full size)
        up_now = ur::DLSS;
        assert(strcmp(tx_resolution(3), "Quality (67%)") == 0 && strcmp(tx_resolution(6), "Ultra Performance (33%)") == 0);
        const unsigned want[][3] = {{3, 1280, 720}, {5, 1120, 630}, {4, 960, 540}, {6, 640, 360}};
        for (const auto& m : want) {
            game_size(ur::percent(m[0]) / 100.0f, scene_w, scene_h);
            assert(scene_w == m[1] && scene_h == m[2] && scene_w * 1080 == scene_h * 1920);
        }
        up_now = ur::DLAA;   // ((Triforce, options_version 7) the Render resolution's Native (100%) step)
        assert(strcmp(tx_resolution(1), "Native (100%)") == 0);
        up_now = ur::GAME;
    }
}
'''
    cases = (
        (0, 0, None, 0, 2560, 1440, "2048x1152"),
        (1, 0, None, 1, 2560, 1440, "2560x1440"),
        (2, 0, None, 2, 3840, 2160, "3840x2160"),
        (3, 0, None, 3, 1920, 1080, "1920x1080"),
        (1, 0, "3", 3, 1920, 1080, "1920x1080"),
        (3, 1, "3", 0, 2560, 1440, "2048x1152"),
        (4, 1, None, 0, 2560, 1440, "2048x1152"),
        (7, 0, None, 0, 2560, 1440, "2048x1152"),
        (5, 0, None, 5, 5120, 2880, "5120x2880"),
        (4, 0, None, 4, 5120, 1440, "5120x1440"),
        (6, 0, None, 6, 5120, 2160, "5120x2160"),
        (1, 0, "6", 6, 5120, 2160, "5120x2160"),
        # ((Triforce, options_version 7) the handheld profile as the Output resolution's 7, saved or for a run)
        (1, 0, "7", 0, 2560, 1440, "2048x1152"),
        (8, 0, None, 0, 2560, 1440, "2048x1152"),
    )
    with tempfile.TemporaryDirectory(prefix="output-resolution-") as tmp:
        work = Path(tmp)
        source, exe = work / "fixture.cpp", work / "fixture.exe"
        source.write_text(fixture, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++17", "-I", str(ROOT), str(source), "-o", str(exe)], check=True)
        for saved, handheld, override, mode, w, h, label in cases:
            (work / "triforce_options.ini").write_text(f"output_res={saved}\nhandheld_profile={handheld}\n", encoding="ascii")
            env = os.environ.copy()
            for key in ("HOS_OUTPUT_RES", "HOS_HANDHELD", "HOS_RT_POOL_MB"):
                env.pop(key, None)
            if override is not None:
                env["HOS_OUTPUT_RES"] = override
            subprocess.run([str(exe), str(mode), str(w), str(h), label], cwd=work, env=env, check=True)
    print("PASS: output sizes, menu order, 1080p override, handheld, render-target budgets and DLSS sizes at 1080p, 5120x1440, 5120x2160 and 5120x2880")


if __name__ == "__main__":
    main()
