"""Our tabs over the game's own Options screen name the buttons in the face the game's own prompts there have
(gpu/vk/vk_menu.cpp draw_game_tab, game_prompts_pad; no game).

(Triforce, 11-19) The lab's "Options screen: the tab strip's ends show LB/RB pad icons while the footer shows the keyboard's
Shift (keyboard face active)". On the game's own tab the footer under our tabs is the game's (Default Settings, Back), in the
face host/prompts.c gave the game at its start; the strip's ends took the overlay's own device used last (pad_last), which a
test's scripted press on the Mods entry sets as a pad's. This compiles game_prompts_pad with a fake host and checks it is a
controller's face unless the game has the keys' (the game's own icons, -1, are the Switch's), and reads draw_game_tab for
the strip drawn with it.

    python -I -B tests/test_game_tab_prompts.py [--cxx clang++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"
RUN_ALL_ARGS = ["--cxx", "{cxx}"]


def check(ok, why):
    if not ok:
        raise SystemExit("FAIL: " + why)


def block(text, head):
    start = text.index(head)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


PROGRAM = r"""
#include <cstdio>
#include "vk_api.h"
static int face = -1;
static int answer() { return face; }
struct { int (*prompt_face)(void); } fake{answer}, *host = &fake;
%s
int main() {
    int fails = 0;
    for (int f = -1; f < HOS_PROMPT_N; f++) {
        face = f;
        if (game_prompts_pad() != (f != HOS_PROMPT_KEYBOARD)) std::printf("FAIL: face %d\n", f), fails++;
    }
    host = nullptr;
    if (!game_prompts_pad()) std::printf("FAIL: no host\n"), fails++;
    if (!fails) std::puts("the game's prompts' face");
    return fails ? 1 : 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")
    tab = block(menu, "void draw_game_tab(ui::Ctx& c) {")
    check("k.pad = game_prompts_pad();" in tab and tab.index("k.pad = game_prompts_pad();") < tab.index("draw_tabs(k, -1, true, true);"),
          "the tab strip over the game's own tab does not name the buttons in the game's prompts' face")
    with tempfile.TemporaryDirectory(prefix="game-tab-prompts-") as tmp:
        src, exe = Path(tmp) / "face.cpp", Path(tmp) / "face.exe"
        src.write_text(PROGRAM.replace("%s", block(menu, "bool game_prompts_pad() {"), 1), encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + str(VK), str(src), "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        check(r.returncode == 0, "the game's prompts' face:\n" + r.stdout)
        print(r.stdout.strip())
    print("PASS: the tab strip over the game's Options names the buttons as the game's own footer there")


if __name__ == "__main__":
    main()
