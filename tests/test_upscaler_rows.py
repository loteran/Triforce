"""The Insert menu's scaling rows: the Upscaler (opts.dlss: the game's own, NVIDIA DLSS and DLAA, AMD FSR 3.1, AMD FSR 4 and
its native AA) and the Render resolution's steps for it (gpu/vk/upscaler_rows.h), their order on the Graphics tab, options
files from before still loading, and their help (why DLSS or FSR 4 is not offered, that Sharpness sharpens DLSS's 3D). No game or GPU
required.

python -I -B tests/test_upscaler_rows.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function(source, start):
    """A function's body from its first line to its closing brace at the start of a line"""
    at = source.find(start)
    if at < 0:
        raise SystemExit("%s is missing" % start.rstrip(" {("))
    body = source[at:]
    return body[:body.index("\n}\n")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    # ((Triforce, options_version 7) the Graphics tab's groups: Upscaling (the Upscaler, the DLSS model, the Render resolution,
    # Upscale before bloom, Sharpness), Anti-aliasing, Frame generation, Frame rate & latency; the Output resolution is the
    # Display tab's Screen group's, before them all. The Window scaling filter is gone: the sizes choose it)
    order = ['"Output resolution", &opts.output_res', 'MENU_HEADING(TAB_GRAPHICS, "Upscaling")', '"Upscaler", &opts.dlss',
             '"DLSS model", &opts.dlss_model', '"Render resolution", &opts.resolution', '"Upscale before bloom", &opts.upscale_pre',
             '"Sharpness", &opts.sharpness', 'MENU_HEADING(TAB_GRAPHICS, "Frame generation")', '"Frame generation", &merged.framegen',
             'MENU_HEADING(TAB_GRAPHICS, "Frame rate & latency")', '"Frame pacing", &merged.pacing', '"Low latency", &merged.latency']
    at = [menu.find(row) for row in order]
    if -1 in at:
        raise SystemExit("vk_menu.cpp: a scaling row is missing: " + order[at.index(-1)])
    if at != sorted(at):
        raise SystemExit("vk_menu.cpp: the scaling rows are not in the picture's order")
    if '"NVIDIA DLSS", &opts.' in menu or '"AMD FSR 4", &opts.' in menu or "&opts.upscale," in menu:
        raise SystemExit("vk_menu.cpp: DLSS or FSR has a row of its own beside the Upscaler, or the window's filter is back")
    # saved options: the renamed rows keep their keys (data, so read from the source)
    key = function(menu, "const char* ini_key(const Row& r) {")
    for value, name in (("opts.resolution", "resolution"), ("opts.dlss", "dlss")):
        if not re.search(r"&%s\)\s*return\s*\"%s\"" % (re.escape(value), name), key):
            raise SystemExit("ini_key: %s is not saved as %s (an options file from before would lose it)" % (value, name))
    # the menu's rules are upscaler_rows.h's, checked by behaviour in test_upscaler_rows.cpp: here only that the menu uses them
    uses = {
        "row_fixed": ("ur::upscaler_fixed(", "ur::model_fixed("),
        "row_shown": ("ur::upscaler_shown(dlss_built() || fsr4_built(), fsr3::supported() || ffx3_usable())", "ur::model_shown(dlss_built())",
                      "ur::is_dlss(up)"),
        "set_row": ("ur::step_engine(", "ur::step_resolution(", "ur::snap(", "row_fixed("),
    }
    for name, calls in uses.items():
        body = function(menu, "%s %s(" % ("void" if name == "set_row" else "bool", name))
        for call in calls:
            if call not in body:
                raise SystemExit("%s does not use %s (upscaler_rows.h)" % (name, call))
    # the help: a row's own, then a line for the value chosen (Row::hint). The Upscaler's says why DLSS is not offered, and
    # DLSS's values that Sharpness sharpens their 3D before the HUD ((Triforce, S7) no longer "set Sharpness to around 80%",
    # which was the window's sharpening, the HUD with it)
    upscaler = function(menu, "const char* hn_upscaler(int v, int variant) {")
    for why in ("not in this build", "RX_VK_DLSS=0", "NVIDIA RTX GPU", "newer NVIDIA driver", "runtime was not found",
                "(int)dlss_why()", "RX_VK_FSR4=0", "AMD's files were not found", "no FSR 4 for this GPU", "(int)fsr4_why()"):
        if why not in upscaler:
            raise SystemExit("hn_upscaler: the help no longer says why DLSS or FSR is not offered (%s)" % why)
    # ((Triforce, options_version 7) no upscaler's line tells the player what to set Sharpness to: that is the Sharpness row's.
    # The Upscaler's lines are one per engine (DLAA reads as DLSS's), and DLSS's says Sharpness sharpens its 3D before the HUD)
    resolution = function(menu, "const char* hn_resolution(int v, int variant) {")
    if "Sharpness to" in upscaler or "Sharpness to" in resolution or "80%" in upscaler or "upscaler()" not in resolution:
        raise SystemExit("hn_upscaler or hn_resolution: a line still asks for a Sharpness, or not for the Upscaler shown")
    if upscaler.count("Sharpness sharpens its 3D before the HUD") != 1 or "v = ur::engine(v);" not in upscaler:
        raise SystemExit("hn_upscaler: NVIDIA DLSS (and DLAA, its engine's line) does not say that Sharpness sharpens its 3D "
                         "before the HUD")
    measure = function(menu, "float measure_help(ui::Ctx& c, float width) {")
    if "r.hint(v, k)" not in measure:
        raise SystemExit("measure_help leaves the values' lines out: the help's room would jump as a value changes")
    for row, hint in (('"Upscaler", &opts.dlss', "hn_upscaler"), ('"DLSS model", &opts.dlss_model', "hn_dlss_model"), ('"Render resolution", &opts.resolution', "hn_resolution"),
                      ('"Low latency", &merged.latency', "hn_latency"), ('"Frame generation", &merged.framegen', "hn_framegen_row")):
        entry = menu[menu.index(row):]
        if hint not in entry[:entry.index("},")]:
            raise SystemExit("vk_menu.cpp: %s has no %s" % (row.split(",")[0], hint))
    # the DLSS model row: not the Quality preset's (preset_exempt), no restart, and its preset is what DLSS is made with
    model = menu[menu.index('"DLSS model", &opts.dlss_model'):]
    if not model[:model.index("},")].rstrip().endswith("hn_dlss_model, false, true"):
        raise SystemExit("vk_menu.cpp: the DLSS model row takes a restart or turns the Quality preset to Custom")
    # the window's filter's sizes: only from the game's frames. The start-up screen is drawn at the window's size
    # (present_image with no source): taken as the picture's, the filter would follow it
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    present = function(replay, "void present_image(")
    note = present.index("menu_note_present_sizes(")
    if "if (src) {" not in present[:note] or present.rindex("if (src) {", 0, note) < present.rindex("\n    }", 0, note):
        raise SystemExit("present_image: the start-up screen's frames (no src) count as the game's picture for the filter row")
    dlss = (ROOT / "gpu" / "vk" / "vk_dlss.cpp").read_text(encoding="utf-8")
    feature = function(dlss, "int prepare(")
    if "wanted_preset()" not in feature or "s.feature_preset == preset" not in feature:
        raise SystemExit("vk_dlss.cpp: DLSS's feature is not made again for another DLSS model")
    if "upscaler_rows::dlss_preset(opts.dlss_model" not in function(dlss, "unsigned wanted_preset() {"):
        raise SystemExit("vk_dlss.cpp: the DLSS model row (and RX_VK_DLSS_PRESET over it) is not the preset asked for")
    temporal = (ROOT / "gpu" / "vk" / "vk_temporal.cpp").read_text(encoding="utf-8")
    mode = function(temporal, "int temporal_mode() {")
    if "dlss_why() != DLSS_OK" not in mode or "fsr4_why() != FSR_OK" not in mode:
        raise SystemExit("temporal_mode asks for an engine where it cannot run (DLAA or FSR 4's native AA would hold the 3D at "
                         "full scale for nothing)")
    if "std::clamp(" in mode:
        raise SystemExit("temporal_mode clamps the row's value: FSR 3.1 (3) would run on the chain")
    if "fsr3_wanted()" not in mode:
        raise SystemExit("temporal_mode runs DLSS or FSR 4 while FSR 3.1 upscales (two upscalers, two jitters)")
    fsr3 = (ROOT / "gpu" / "vk" / "vk_fsr3.cpp").read_text(encoding="utf-8")
    # (linux fork: read in fsr3_requested, which fsr3_wanted and AMD's library share)
    if "!temporal_env_chosen()" not in function(fsr3, "bool fsr3_requested() {") or "fsr3_requested()" not in function(fsr3, "bool fsr3_wanted() {"):
        raise SystemExit("fsr3_wanted: a saved FSR 3.1 drops RX_VK_DLSS / RX_VK_FSR4, which go over the row")
    chosen = function(temporal, "bool temporal_env_chosen() {")
    if "dlss_why() == DLSS_OK" not in chosen or "fsr4_why() == FSR_OK" not in chosen:
        raise SystemExit("temporal_env_chosen: a switch for an engine that cannot run here leaves the game's own, not the saved FSR 3.1")
    # FSR 3.1 and FSR 4 are offered where they run, and the row steps over what does not run here
    available = function(menu, "unsigned upscalers_available() {")
    if "fsr3::supported()" not in available:
        raise SystemExit("upscalers_available does not ask vk_fsr3.cpp whether FSR 3.1 runs here")
    if "fsr4_why() == FSR_OK" not in available:
        raise SystemExit("upscalers_available does not ask vk_fsr4.cpp whether FSR 4 runs here")
    if "upscalers_available()" not in function(menu, "void set_row(") or "upscalers_available()" not in function(menu, "bool row_fixed("):
        raise SystemExit("the Upscaler row does not step over (or fix on) what runs here")
    # the FSR sharpening and Frame generation engine rows: beside the Upscaler and Frame generation, the player's (not the preset's), the engine
    # row's choice taken through the engine switch and fixed by RX_VK_FRAMEGEN_VENDOR
    for row, hint in (('"FSR sharpening", &opts.fsr_sharpness', "hn_fsr_sharpness"), ('"Frame generation engine", &opts.fg_engine', "hn_fg_engine")):
        if row not in menu:
            raise SystemExit("vk_menu.cpp: the %s row is missing" % row.split(",")[0])
        entry = menu[menu.index(row):]
        entry = entry[:entry.index("},")]
        if hint not in entry or not entry.rstrip().endswith("false, true"):
            raise SystemExit("vk_menu.cpp: %s has no %s, or takes a restart, or turns the Quality preset to Custom" % (row.split(",")[0], hint))
    # (v0.6.0: the Frame generation and Frame pacing rows are upstream's merged ones, merged_rows.h)
    if menu.index('"Frame generation", &merged.framegen') > menu.index('"Frame generation engine", &opts.fg_engine') or \
            menu.index('"Frame generation engine", &opts.fg_engine') > menu.index('"Frame pacing", &merged.pacing'):
        raise SystemExit("vk_menu.cpp: the Frame generation engine row is not between Frame generation and Frame pacing")
    if "ngxfg::vendor_env_set()" not in function(menu, "bool row_fixed(") or "ur::fsr_sharpen_fixed(" not in function(menu, "bool row_fixed("):
        raise SystemExit("row_fixed: the engine row is not fixed by RX_VK_FRAMEGEN_VENDOR, or FSR sharpening by the Upscaler")
    if "fgengine::step_row(" not in function(menu, "void set_row("):
        raise SystemExit("set_row: the engine row does not step over what cannot run here")
    for name in ("vk_ffx3.cpp", "vk_fsr4.cpp"):
        if "fsr_sharpen::strength(opts.fsr_sharpness" not in (ROOT / "gpu" / "vk" / name).read_text(encoding="utf-8"):
            raise SystemExit("%s does not give AMD's library the FSR sharpening row's strength" % name)
    with tempfile.TemporaryDirectory(prefix="upscaler-rows-") as tmp:
        exe = Path(tmp) / "upscaler-rows"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests" / "test_upscaler_rows.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
