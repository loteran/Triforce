"""A scissor's rectangle without integer overflow (gpu/vk/rect_clamp.h; no game, no GPU).

NVN's scissor is four ints the game gives. vk_draw.cpp summed x + width in an int: a large width (one that means "to the
edge") wrapped negative: a clear's rectangle (clear_rect) came out empty, and the draw's scissor (dynamic_state) wrapped
again in its end minus its start, to an extent of some two billion pixels (past the target, which Vulkan does not allow). The
pre-tonemap retarget in draw() scaled the scissor by a float and converted it to an int unclamped: a width scaled past
an int's range became INT_MIN (x86), and the scissor then counted as none.

Compiles and runs rect_clamp.h against the expressions it replaced (under -fwrapv, the wrap a build gets): the inputs
that wrapped them come out right now, and every input that did not wrap gives what it gave before. Reads vk_draw.cpp
for the three call sites and that none of the old expressions is left.

    python -I -B tests/test_rect_clamp.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]

PROGRAM = r'''
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include "rect_clamp.h"

static int failed;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failed = 1; } } while (0)

// the expressions as they were in vk_draw.cpp
static rect_clamp::Span old_scissor(int32_t x, int32_t w, uint32_t cap) {
    const int32_t x0 = std::max(0, x);
    const int32_t x1 = std::min<int32_t>(x + w, (int32_t)cap);
    return {x0, (uint32_t)std::max(0, x1 - x0)};
}
static rect_clamp::Span old_clear(int32_t x, int32_t w, uint32_t cap) {
    const int32_t x0 = std::clamp(x, 0, (int32_t)cap);
    const int32_t x1 = std::clamp(x + w, 0, (int32_t)cap);
    return {x0, (uint32_t)std::max(0, x1 - x0)};
}
static bool same(rect_clamp::Span a, rect_clamp::Span b) { return a.at == b.at && a.n == b.n; }

int main() {
    // a scissor from 8 to the edge, said with INT_MAX: the old sum wrapped and the draw drew nothing
    CHECK(old_scissor(8, INT32_MAX, 1920).n > 1u << 30);   // (x1 wrapped negative, and x1 - x0 wrapped back to INT_MAX)
    CHECK(same(rect_clamp::scissor(8, INT32_MAX, 1920), {8, 1912}));
    CHECK(old_clear(8, INT32_MAX, 1920).n == 0);
    CHECK(same(rect_clamp::clear(8, INT32_MAX, 1920), {8, 1912}));
    CHECK(same(rect_clamp::scissor(INT32_MAX, INT32_MAX, 1920), {INT32_MAX, 0}));
    CHECK(same(rect_clamp::clear(INT32_MAX, INT32_MAX, 1920), {1920, 0}));
    CHECK(same(rect_clamp::scissor(-100, INT32_MAX, 1080), {0, 1080}));
    // everywhere the old sum did not wrap: the same rectangle
    const int32_t xs[] = {INT32_MIN, -100000, -1921, -1, 0, 1, 7, 959, 1919, 1920, 1921, 5000, 1 << 30};
    const int32_t ws[] = {1, 2, 100, 1919, 1920, 1921, 4096, 1 << 20, 1 << 29};
    const uint32_t caps[] = {1, 720, 1920, 16384};
    for (int32_t x : xs)
        for (int32_t w : ws)
            for (uint32_t cap : caps) {
                if ((int64_t)x + w > INT32_MAX) continue;
                CHECK(same(rect_clamp::scissor(x, w, cap), old_scissor(x, w, cap)));
                CHECK(same(rect_clamp::clear(x, w, cap), old_clear(x, w, cap)));
            }
    // the retarget's scale: a cast in range as it was, out of range clamped (a cast gave INT_MIN on x86)
    CHECK(rect_clamp::px(2147483647.0f * 1.5f) == INT32_MAX);
    CHECK(rect_clamp::px(1e30f) == INT32_MAX && rect_clamp::px(-1e30f) == INT32_MIN && rect_clamp::px(0.0f / 0.0f) == 0);
    for (float v : {0.0f, 0.5f, 1.49f, 1919.5f, -3.7f, 16777216.0f, 2147483520.0f, -2147483648.0f})
        CHECK(rect_clamp::px(v) == (int32_t)v);
    if (!failed) std::printf("PASS: scissors and clears whose x + width wrapped an int are right, the others as before\n");
    return failed;
}
'''


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check_source():
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    if '#include "rect_clamp.h"' not in draw:
        raise SystemExit("FAIL: gpu/vk/vk_draw.cpp does not include rect_clamp.h")
    state = block(draw, "void dynamic_state(VkCommandBuffer cmd")
    if ("rect_clamp::scissor(s.scissor[0], s.scissor[2], at.width)" not in state or
            "rect_clamp::scissor(s.scissor[1], s.scissor[3], at.height)" not in state):
        raise SystemExit("FAIL: vk_draw.cpp dynamic_state does not take its scissor from rect_clamp::scissor")
    clear = block(draw, "VkRect2D clear_rect(")
    if "rect_clamp::clear(s.scissor[0], s.scissor[2], w)" not in clear or "rect_clamp::clear(s.scissor[1], s.scissor[3], h)" not in clear:
        raise SystemExit("FAIL: vk_draw.cpp clear_rect does not take its rectangle from rect_clamp::clear")
    for old in ("s.scissor[0] + s.scissor[2]", "s.scissor[1] + s.scissor[3]"):
        rest = draw.replace("(int64_t)" + old, "")
        if old in rest:
            raise SystemExit("FAIL: vk_draw.cpp still sums %s in an int" % old)
    if "(int32_t)((float)sc[" in draw or draw.count("rect_clamp::px((float)sc[") != 4:
        raise SystemExit("FAIL: vk_draw.cpp draw's retarget converts the scaled scissor without rect_clamp::px")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="rect-clamp-") as tmp:
        cpp = Path(tmp) / "rect.cpp"
        exe = Path(tmp) / "rect.exe"
        cpp.write_text(PROGRAM, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-O2", "-fwrapv", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu/vk"), str(cpp),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
