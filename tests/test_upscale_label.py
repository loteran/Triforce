"""The window's filter: (Triforce, options_version 7) no row any more, the picture's size and its place in the window choose it
(gpu/vk/upscaler_rows.h auto_filter, at each present: vk_menu.cpp menu_note_present_sizes), and RX_VK_UPSCALE stays inside
the filters there are. Before, the Window scaling filter row's label had to stay inside its names.

python -I -B tests/test_upscale_label.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
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
    source = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    if "const char* tx_upscale(int v) {" in source or '"Window scaling filter"' in source:
        raise SystemExit("the Window scaling filter row is back beside the filter the sizes choose")
    env = source[source.index('if (const char* e = getenv("RX_VK_UPSCALE"))'):source.index('getenv("HOS_REALTIME_STEP")')]
    if "opts.upscale = std::min(std::max(atoi(e), 0), 3);" not in env:
        raise SystemExit("RX_VK_UPSCALE is stored past the filters there are (0 to 3)")
    note = block(source, "void menu_note_present_sizes(")
    # (and with HDR Native the plain copy: the game's picture is 10-bit PQ, and framegen_scale's images are rgba8)
    for need in ('static const bool fixed = getenv("RX_VK_UPSCALE") != nullptr;', "if (fixed) return;",
                 "present_rect(picture_w, picture_h, nullptr, window_w, window_h, aspect)",
                 "const bool native_hdr = dev.hdr && opts.hdr == 2;",
                 "ur::window_filter(picture_w, picture_h, place.w, place.h, native_hdr)", "opts.upscale = filter;"):
        if need not in note:
            raise SystemExit("menu_note_present_sizes no longer has: " + need)
    if "ur::auto_filter(" in note:
        raise SystemExit("menu_note_present_sizes chooses the filter without HDR Native's plain copy (upscaler_rows.h window_filter)")
    # vk_replay.cpp's native PQ (the picture that skips framegen_hdr) is always the case window_filter is given
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    if "const bool native_pq = tex && dev.hdr && opts.hdr == 2 && " not in replay:
        raise SystemExit("vk_replay.cpp's HDR Native picture is no longer dev.hdr && opts.hdr == 2: menu_note_present_sizes's test")
    # AMD FSR 1, which the sizes now choose, sharpens only with Sharpness above 0: at 0, EASU alone into the image shown
    # (fsr1_rcas.hlsl's 0% is 2 stops below its strongest, not off)
    scale = block((ROOT / "gpu" / "vk" / "vk_framegen.cpp").read_text(encoding="utf-8"), "VkImage framegen_scale(")
    # ((Triforce, S1, S3) the window's Sharpness is sharpen_rules.h window_sharpness's: 0 at the row's 0, and 0 while the
    # upscaler sharpened the 3D before the HUD; EASU then writes the image shown, and RCAS runs only under if (sharp))
    if "const int sharp = sharpen_rules::window_sharpness(opts.sharpness, temporal_sharpens_3d());" not in scale:
        raise SystemExit("framegen_scale: the window's Sharpness is not window_sharpness's (0 at the row's 0)")
    fsr1 = block(scale, "if (fsr1) {")
    easu = "pass(cmd, easu, {{src, scale.linear}, {sharp ? scale.easu_img.view : scale.scaled.view, VK_NULL_HANDLE}}"
    if easu not in fsr1 or "if (sharp) {" not in fsr1 or "return scale.scaled.image;" not in fsr1:
        raise SystemExit("framegen_scale: AMD FSR 1 at Sharpness 0 is not EASU alone into the image shown")
    on = block(fsr1, "if (sharp) {")
    if "pass(cmd, rcas," not in on or fsr1.count("pass(cmd, rcas,") != 1 or fsr1.index(easu) > fsr1.index("if (sharp) {"):
        raise SystemExit("framegen_scale: RCAS runs outside the Sharpness above 0 case")
    fixture = r'''#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include "''' + (ROOT / "gpu" / "vk" / "upscaler_rows.h").as_posix() + r'''"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
int main() {
    namespace ur = mkwvk::upscaler_rows;
    /* (a filter of those there are, vk_framegen.cpp framegen_scale's: 0 the plain copy, 3 AMD FSR 1 when the picture is
       enlarged; the plain copy, the linear blit, when it is made smaller; and with HDR Native always the plain copy) */
    for (unsigned pw : {0u, 1536u, 2048u, 2560u, 3840u})
        for (unsigned dw : {0u, 1280u, 2560u, 3440u, 5120u}) {
            const int f = ur::auto_filter(pw, pw * 9 / 16, dw, dw * 9 / 16);
            CHECK(f == 0 || f == 3);
            CHECK((f == 3) == (pw && dw > pw));
            CHECK(ur::window_filter(pw, pw * 9 / 16, dw, dw * 9 / 16, false) == f);
            CHECK(ur::window_filter(pw, pw * 9 / 16, dw, dw * 9 / 16, true) == 0);
        }
    /* the owner's: a 2560 x 1440 picture of 21:9 shown in 3360 x 1440 of a 3440 x 1440 window, HDR Native + RenoDX: the
       10-bit picture blitted as it is; in SDR, AMD FSR 1. The defaults (2048 x 1152) on a Steam Deck's 1280 x 800: the blit */
    CHECK(ur::window_filter(2560, 1440, 3360, 1440, true) == 0 && ur::window_filter(2560, 1440, 3360, 1440, false) == 3);
    CHECK(ur::window_filter(2048, 1152, 1280, 720, false) == 0 && ur::window_filter(3840, 2160, 2560, 1440, false) == 0);
    std::printf("PASS: the window's filter is one of those there are, none at the picture's own size or with HDR Native\n");
}
'''
    with tempfile.TemporaryDirectory(prefix="upscale-label-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
