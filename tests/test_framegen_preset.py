"""The Insert menu's Frame generation row (Triforce, options_version 7): one row for the renderer's own made frames, AMD FSR 4's
and NVIDIA DLSS's, which was a row of its own (gpu/vk/merged_rows.h). Before, this checked that a Quality preset left the
row alone; the preset is gone.

The menu's own set_row, merged_step, merged_store, merged_sync and framegen_row_sync are compiled with small stand-ins and
stepped as the arrows step them: the choices offered are those that run here, a DLSS value sets dlss_frame_generation and
turns the renderer's own off, and the restart is asked for DLSS's alone. The Frame rate cap keeps its saved key. No game or
GPU is needed.
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, start):
    opening = text.index("{", start)
    depth = 0
    for at in range(opening, len(text)):
        depth += text[at] == "{"
        depth -= text[at] == "}"
        if depth == 0:
            return text[start:at + 1]
    raise SystemExit("unterminated block at %d" % start)


def definition(text, name):
    match = re.search(re.escape(name) + r"\([^;{]*\)\s*\{", text)
    if not match:
        raise SystemExit("missing definition: " + name)
    return block(text, match.start())


FIXTURE = r"""
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include "@ROOT@/gpu/vk/merged_rows.h"
#include "@ROOT@/gpu/vk/framegen_rows.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)
namespace mkwvk {
/* OPTIONS */
Options opts;
/* TABS */
/* ROW */
namespace mr7 = merged_rows;
/* MERGED */
int saves = 0;
void save() { ++saves; }
void apply_pad_layout(int) {}
bool row_fixed(const int*) { return false; }
int upscaler() { return 0; }
unsigned upscalers_available() { return 1; }
/* (the Frame generation engine row's choices, which set_row steps through: none here) */
unsigned fg_engines_available() { return 1; }
namespace fgengine { [[maybe_unused]] constexpr int kEngineRows = 4; int step_row(int v, int, unsigned) { return v; } }
namespace ur {
struct Scaling { int upscaler, resolution; };
Scaling step_engine(int, int v, int, unsigned) { return {0, v}; }
Scaling step_resolution(int, int v, int dir) { return {0, v + dir}; }
int snap(int, int value) { return value; }
bool full_scale(int) { return false; }
}
namespace mr { constexpr int kMax = 8; }
void restart_values(int*) {}
struct { void note(const int*) {} } restart_state;
bool can_mic() { return false; }
void mic_chosen() {}
int step_output_choice(int id, int dir) { return id + dir; }
const char* tx_framegen_row(int) { return ""; }
const char* hn_framegen_row(int, int) { return ""; }
const char* tx_fps_cap(int) { return ""; }
/* (what runs here: AMD FSR 4's frame generation, DLSS frame generation, the card's maker for Low latency) */
bool fsr4_here = false, dlssg_here = false;
namespace fsr4fg { bool offered_known() { return fsr4_here; } }
bool dlssg_offered() { return dlssg_here; }
bool renodx_built() { return true; }
mr7::Vendor latency_vendor() { return mr7::LATENCY_AMD; }
/* LISTS */
/* SYNC */
/* MERGED_FUNCTIONS */
/* INI_KEY */
/* STOPS */
/* RESTART_VALUE */
/* SET_ROW */
}  // namespace mkwvk
using namespace mkwvk;

int main() {
    Row framegen = /* FRAMEGEN_ROW */;
    Row fps_cap = /* FPS_CAP_ROW */;
    CHECK(std::strcmp(fps_cap.name, "Frame rate cap") == 0);
    CHECK(std::strcmp(ini_key(fps_cap), "frame_rate_cap") == 0);
    CHECK(std::strstr(fps_cap.help, "made frames come on top") != nullptr);
    CHECK(framegen.value == &merged.framegen && framegen.restart && !ini_row(framegen));
    CHECK(opts.framegen == 0 && opts.dlssg == 0);
    /* without FSR 4's and DLSS's frame generation here: Off, Auto, 2x to 8x, round */
    framegen_row_sync();
    merged_sync();
    for (int want : {1, 2, 3, 4, 5, 6, 7, 8, 0}) {
        set_row(framegen, *framegen.value + 1, 1);
        CHECK(opts.framegen == want && opts.dlssg == 0 && merged.framegen == want && restart_value_of(framegen) == 0);
    }
    /* with both: 8x, AMD FSR 4, NVIDIA DLSS 2x, 3x, 4x, then Off; DLSS's turn the renderer's own off, and take the restart */
    fsr4_here = dlssg_here = true;
    framegen_row_sync();
    opts.framegen = 8, merged_sync();
    set_row(framegen, *framegen.value + 1, 1);
    CHECK(opts.framegen == framegen_rows::FSR4 && opts.dlssg == 0);
    for (int dlss = 1; dlss <= 3; dlss++) {
        set_row(framegen, *framegen.value + 1, 1);
        CHECK(opts.dlssg == dlss && opts.framegen == 0 && merged.framegen == mr7::FG_DLSS + dlss - 1 && restart_value_of(framegen) == dlss);
    }
    set_row(framegen, *framegen.value + 1, 1);
    CHECK(opts.dlssg == 0 && opts.framegen == 0);
    set_row(framegen, *framegen.value - 1, -1);   /* (Left from Off: NVIDIA DLSS 4x) */
    CHECK(opts.dlssg == 3 && opts.framegen == 0);
    /* a saved AMD FSR 4 where its library does not run: kept until stepped, the steps from it as from 8x */
    fsr4_here = false;
    framegen_row_sync();
    opts.dlssg = 0, opts.framegen = framegen_rows::FSR4, merged_sync();
    CHECK(merged.framegen == framegen_rows::FSR4);
    set_row(framegen, *framegen.value + 1, 1);
    CHECK(opts.dlssg == 1 && opts.framegen == 0);
    opts.dlssg = 0, opts.framegen = framegen_rows::FSR4, merged_sync();
    set_row(framegen, *framegen.value - 1, -1);
    CHECK(opts.framegen == 8 && opts.dlssg == 0);
    CHECK(saves > 0);
    std::puts("PASS: the Frame generation row steps through what runs here, DLSS's values its own, and the cap keeps its key");
}
"""


def fixture(source):
    menu = (source / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    header = (source / "gpu" / "vk" / "vk_menu.h").read_text(encoding="utf-8")
    options = re.search(r"struct Options \{.*?\n\};", header, re.S)
    tabs = re.search(r"enum MenuTab \{.*?\};", menu, re.S)
    row = re.search(r"struct Row \{.*?\n\};", menu, re.S)
    if not options or not tabs or not row:
        raise SystemExit("the menu's options, tabs, or row definition is missing")
    fps_cap = re.search(r'\{TAB_GRAPHICS, "Frame rate cap", &opts\.fps_cap,', menu)
    if not fps_cap:
        raise SystemExit("missing Frame rate cap row")
    merged = menu[menu.index("struct Merged {"):menu.index("bool is_merged(const int* v)")]
    merged += menu[menu.index("bool is_merged(const int* v)"):].split("\n", 1)[0] + "\n"
    lists = menu[menu.index("int framegen_list["):menu.index("bool dlssg_offered() {")]
    # (the saved values' own rows: no tab shows them, and the merged row takes the restart for DLSS's)
    for hidden in ('{TAB_HIDDEN, "Frame generation", &opts.framegen, 0, framegen_rows::kRowMax, 1, tx_framegen, nullptr},',
                   '{TAB_HIDDEN, "DLSS frame generation", &opts.dlssg, 0, 3, 1, tx_dlssg, nullptr},'):
        if hidden not in menu:
            raise SystemExit("vk_menu.cpp does not keep a merged value under its key: " + hidden)
    if '"DLSS frame generation", &opts.dlssg, 0, 3, 1, tx_dlssg,\n' in menu:
        raise SystemExit("DLSS frame generation is still a row of its own")
    replacements = {
        "/* OPTIONS */": options.group(0),
        "/* TABS */": tabs.group(0),
        "/* ROW */": row.group(0),
        "/* MERGED */": merged,
        "/* LISTS */": lists,
        "/* SYNC */": definition(menu, "void framegen_row_sync"),
        "/* MERGED_FUNCTIONS */": definition(menu, "void merged_sync") + "\n" + definition(menu, "void merged_store") + "\n" +
                                  definition(menu, "int merged_step"),
        "/* STOPS */": definition(menu, "bool stops_at_ends"),
        "/* INI_KEY */": definition(menu, "const char* ini_key") + "\n" + definition(menu, "bool ini_row"),
        "/* RESTART_VALUE */": definition(menu, "int restart_value_of"),
        "/* FPS_CAP_ROW */": block(menu, fps_cap.start()),
        "/* SET_ROW */": definition(menu, "void set_row"),
        "/* FRAMEGEN_ROW */": block(menu, menu.index('{TAB_GRAPHICS, "Frame generation"')),
        "@ROOT@": source.as_posix(),
    }
    result = FIXTURE
    for marker, value in replacements.items():
        result = result.replace(marker, value)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="framegen-preset-") as tmp:
        src = Path(tmp) / "framegen_preset.cpp"
        exe = Path(tmp) / ("framegen_preset.exe" if os.name == "nt" else "framegen_preset")
        src.write_text(fixture(args.source), encoding="utf-8")
        if Path(args.cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
            command = [args.cxx, "/nologo", "/std:c++20", "/EHsc", str(src),
                       "/Fo" + str(Path(tmp) / "framegen_preset.obj"), "/Fe" + str(exe)]
        else:
            command = [args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                       str(src), "-o", str(exe)]
        subprocess.run(command, cwd=tmp, check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
