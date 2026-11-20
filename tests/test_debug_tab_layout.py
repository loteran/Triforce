"""The Debug tab's layout (gpu/vk/debug_tab_layout.h, gpu/vk/vk_menu.cpp draw_debug_rows; no game).

Looked at in pictures (tests/ui_preview.cpp), the tab had three faults. Its rows were cut off in the middle of the column
where they scrolled past the first or the last whole row, while every other tab lets them go under the header and toward
the guides. The Developer row with more than two values (Replay helper threads) had a wider cell than the rows around
it, so the cells' left edges did not line up. And a still (a headless shot, the preview) drew every list at its top
whatever was selected, because only a window's frames stepped the scroll's easing.

Compiles and runs tests/debug_tab_layout_test.cpp: the cells' widths with the tab's own measures (one column, every cell
the width the widest asks, the Developer row's among them), the same at 1, 1.5, 2 and 3 times the size (the layout scales
with the window's height: two columns fit at no window size), a long name that takes the room, a column wide enough for
two, and never a cell under its narrowest.

Reads the source for the rest: draw_debug_rows takes its widths from that function, clips and culls its rows by the
column's own clip (not by the first and last whole row), and gives the mouse the same; a still is drawn settled.

    python -I -B tests/test_debug_tab_layout.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def check(ok, why):
    if not ok:
        raise SystemExit("FAIL: " + why)


def function(text, head):
    start = text.index(head)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    draw = function(menu, "void draw_debug_rows(")
    check("void draw_debug_rows(ui::Ctx& c, float cx0, float cx1, float top, float bottom, float clip0, float clip1, int sel) {" in draw
          and "draw_debug_rows(c, cx0, cx1, top, bottom, clip0, clip1, sel);" in function(menu, "void draw_options(ui::Ctx& c) {"),
          "the Debug tab is not given the column's clip")
    check("c.d->PushClipRect(ImVec2(cx0, clip0), ImVec2(cx1, std::max(clip0, clip1)), true);" in draw, "the Debug tab's rows are not clipped as the other tabs'")
    check(draw.count("if (y + pitch <= clip0 || y >= clip1)") == 2 and "<= top || y >= bottom" not in draw,
          "a row or a heading is left out, or cut, at the first or last whole row")
    check("hits.add(x0, std::max(y, clip0), x1, std::min(y + rh, clip1), menu_hit::kRow, k, va.x, vb.x, 0, 0, 0, vm);" in draw,
          "the mouse's rows are not what shows of them")   # ((Triforce) vm: where a toggle's halves meet, tests/test_menu_hit.py)
    check("scroll_rows(TAB_DEBUG, visual_count, sel_at, top, bottom, pitch, &shown)" in draw, "the scroll no longer counts whole rows between top and bottom")
    check("debug_tab::widths(col_w, right - left, name_need, cell_full, cell_tight, span_need)" in draw
          and "if (i == 0 || i >= dev) span_need = std::max(span_need, need(" in draw, "the cells' widths are not debug_tab_layout.h's")
    check("const float width = in_tools ? cell_w[vcol[i]] : span_w;" in draw, "the switch's and the Developer rows' cells are not one width")
    # a still is drawn settled: only menu_record, a window's frames, steps the easing
    image = function(menu, "void menu_record_image(")
    record = function(menu, "void menu_record(VkCommandBuffer cmd, uint32_t index")
    check("if (!ease_live) ease_step = 1.0f;" in image and image.index("if (!ease_live) ease_step = 1.0f;") < image.index("draw_menu(c"),
          "a still does not draw the lists where the selection asks")
    check("ease_live = true;" in record and "ease_live = true" not in image, "the window's frames do not keep their easing")
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "debug_tab_layout_test.exe"
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/debug_tab_layout_test.cpp"), "-o", str(exe)], check=True)
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        if run.returncode or run.stdout.strip() != "ok":
            raise SystemExit("FAIL: tests/debug_tab_layout_test.cpp (exit %d)\n%s%s" % (run.returncode, run.stdout, run.stderr))
    print("PASS: the Debug tab's cells line up, its rows are clipped as the other tabs', and a still is drawn settled")


if __name__ == "__main__":
    main()
