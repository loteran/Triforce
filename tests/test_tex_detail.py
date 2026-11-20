"""Compile and run the texture detail rule (gpu/vk/tex_detail.h; no game/GPU), and the Insert menu's Texture detail row over
it: the row's four values, RX_VK_TEX_DETAIL still over the row, a Quality preset setting it for its card's size, and what the
row says (gpu/vk/vk_menu.cpp's own functions, cut out and run against stand-ins)."""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    """The texture setup still asks the rule, reads the switch and keeps the textures it must leave whole"""
    src = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    for needle in ('#include "tex_detail.h"', "tex_detail::by_vram(", 'getenv("RX_VK_TEX_DETAIL")',
                   "tex_detail::skip(detail_level(), x.width, x.height, x.levels)",
                   "!as_target && astc && !x.sparse",
                   # the row: as the options file had it at the start, under the switch and over the card's own level
                   "tex_detail::from_row(tex_detail_at_start)", "tex_detail::choose(asked, row, by_memory)"):
        if needle not in src:
            raise SystemExit("vk_texture.cpp no longer has: " + needle)


def block(text, at):
    """text from at to the end of the first {...} after it"""
    start = text.index("{", at)
    depth = 0
    for i in range(start, len(text)):
        depth += text[i] == "{"
        depth -= text[i] == "}"
        if depth == 0:
            return text[at:i + 1]
    raise SystemExit("no end for the block at %d" % at)


def definition(text, head):
    found = re.search(re.escape(head) + r"\([^;{]*\)\s*\{", text)
    if not found:
        raise SystemExit("vk_menu.cpp: no definition of " + head)
    return block(text, found.start())


MENU_FIXTURE = r"""
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "@ROOT@/gpu/vk/tex_detail.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
@OPTIONS@
Options opts;
/* (stand-ins for what the preset asks of the Upscaler's rows, and the card's own level as vk_texture.cpp says it) */
int upscaler() { return 0; }
namespace ur { int snap(int, int v) { return v; } }
int card = -1;
int texture_detail_auto() { return card; }
@BUDGETS@
@PRESET@
@ENV@
@TEXT@
@HINT@
int main(int argc, char**) {
    const bool env = argc > 1;   /* (run again with RX_VK_TEX_DETAIL=low) */
    /* a Quality preset sets its card size's detail, whatever was chosen: Minimum (6GB) Low, Low (8GB) Reduced, Balanced,
       High and Ultra Full; Custom leaves the row alone */
    static const int want[] = {0, 3, 2, 1, 1, 1};
    for (int p = 1; p <= 5; p++)
        for (int v = 0; v <= 3; v++) {
            opts = Options{};
            opts.tex_detail = v;
            apply_preset(p);
            CHECK(opts.tex_detail == want[p]);
        }
    opts = Options{};
    CHECK(opts.tex_detail == 0);   /* (Automatic unless chosen: as before the row) */
    opts.tex_detail = 2;
    apply_preset(0);
    CHECK(opts.tex_detail == 2);
    /* the value: the three levels by name, Automatic with what it gives this card once that is known */
    CHECK(!std::strcmp(tx_tex_detail(1), "Full") && !std::strcmp(tx_tex_detail(2), "Reduced") && !std::strcmp(tx_tex_detail(3), "Low"));
    CHECK(!std::strcmp(tx_tex_detail(0), "Automatic"));
    card = tex_detail::kFull;
    CHECK(!std::strcmp(tx_tex_detail(0), "Automatic (Full)"));
    card = tex_detail::kReduced;
    CHECK(!std::strcmp(tx_tex_detail(0), "Automatic (Reduced)") && !std::strcmp(tx_tex_detail(1), "Full"));
    card = tex_detail::kLow;
    CHECK(!std::strcmp(tx_tex_detail(0), "Automatic (Low)"));
    CHECK(!std::strcmp(tx_tex_detail(7), "Automatic (Low)") && !std::strcmp(tx_tex_detail(-1), "Automatic (Low)"));   /* (past the row) */
    /* the line under the help: one for each value, and the switch's when it is set (every one ends: the help's height) */
    for (int v = 0; v <= 3; v++) {
        int n = 0;
        while (hn_tex_detail(v, n)) n++;
        CHECK(n == 4 && !hn_tex_detail(v, 4) && !hn_tex_detail(v, 5));
        CHECK(std::strstr(hn_tex_detail(v, 3), "RX_VK_TEX_DETAIL=low"));
        const char* now = hn_tex_detail(v, -1);
        CHECK(now && (env ? now == hn_tex_detail(v, 3) : now == hn_tex_detail(v, 0)));
    }
    CHECK(std::strstr(hn_tex_detail(0, 0), "video memory") && std::strstr(hn_tex_detail(1, 0), "whole"));
    std::puts(env ? "tex detail row: the switch's line" : "tex detail row: all checks passed");
    return 0;
}
"""


def menu_fixture():
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    header = (ROOT / "gpu/vk/vk_menu.h").read_text(encoding="utf-8")
    # the row: on the Graphics tab, Automatic to Low, a restart, and not the preset's to keep (no preset_exempt: a change turns
    # the Quality preset to Custom, whose memory limits are the card's own)
    row = re.search(r'\{TAB_GRAPHICS, "Texture detail", &opts\.tex_detail, 0, 3, 1, tx_tex_detail,', menu)
    if not row:
        raise SystemExit("vk_menu.cpp: the Texture detail row is missing, or not Automatic, Full, Reduced, Low on the Graphics tab")
    entry = block(menu, row.start())
    if not entry.rstrip().endswith("hn_tex_detail, true}"):
        raise SystemExit("vk_menu.cpp: the Texture detail row no longer takes a restart, or lost its line about the value")
    if "Applies after a restart" not in entry:
        raise SystemExit("vk_menu.cpp: the Texture detail row's help does not say that it applies after a restart")
    if "tex_detail_at_start = opts.tex_detail;" not in definition(menu, "void menu_init"):
        raise SystemExit("menu_init does not keep the Texture detail the game started with")
    fixture = MENU_FIXTURE.replace("@ROOT@", ROOT.as_posix())
    for mark, text in (("@OPTIONS@", re.search(r"struct Options \{.*?\n\};", header, re.S).group(0)),
                       ("@BUDGETS@", definition(menu, "void sync_preset_budgets")),
                       ("@PRESET@", definition(menu, "void apply_preset")),
                       ("@ENV@", definition(menu, "int tex_detail_env")),
                       ("@TEXT@", definition(menu, "const char* tx_tex_detail")),
                       ("@HINT@", definition(menu, "const char* hn_tex_detail"))):
        fixture = fixture.replace(mark, text)
    return fixture


def check_docs():
    """The row is in the readme's settings table and its numbers in the development guide's"""
    readme = (ROOT / "readme.md").read_text(encoding="utf-8")
    if "| Texture detail | Automatic, Full, Reduced, Low" not in readme:
        raise SystemExit("readme.md's settings table has no Texture detail row")
    guide = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    if "| `texture_detail` | 0 Automatic, 1 Full, 2 Reduced, 3 Low |" not in guide:
        raise SystemExit("docs/development.md's options table has no texture_detail")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    check_docs()
    with tempfile.TemporaryDirectory(prefix="tex-detail-") as tmp:
        exe = Path(tmp) / "tex-detail"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/test_tex_detail.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        # the menu's side: its own functions against stand-ins, without the switch and with it
        source, row = Path(tmp) / "row.cpp", Path(tmp) / "tex-detail-row"
        source.write_text(menu_fixture(), encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(row)], check=True)
        env = {k: v for k, v in os.environ.items() if k != "RX_VK_TEX_DETAIL"}
        subprocess.run([str(row)], check=True, env=env)
        subprocess.run([str(row), "env"], check=True, env=dict(env, RX_VK_TEX_DETAIL="low"))


if __name__ == "__main__":
    main()
