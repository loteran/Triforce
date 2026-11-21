"""RenoDX HDR's rows for RenoDRT and NX2navi's other HDR settings (gamma correction, match tone slope, tone slope, hue clip,
and RenoDRT's highlight saturation, blowout, flare, hue correction and colour grading), without the game, a GPU, RenoDX's
sources or the Slang compiler:

  - the rows: their place on the Display tab, ranges and steps (NX2navi's), their defaults, the keys they are saved under
    (gpu/vk/vk_menu.cpp's own ini_key, cut out and run), and which show with which tonemapper (its rdx_row and rdx_applies,
    cut out and run);
  - the settings the shaders are given (gpu/vk/vk_renodx.cpp's own fill_settings and peak_before_gamma, cut out and run
    against gpu/vk/vk_menu.h's own Options): with the new rows out of the box, Vanilla's and PsychoV35's are bit for bit
    what they were before the rows, written out again here from the code as it was; then every row against the field it
    sets, and the peak the tone mappers are given with the gamma correction;
  - that the block the glue reads (gpu/vk/shaders/renodx_glue/triforce_settings.slangi) is the one vk_renodx.cpp fills
    and binds.

What the shaders do with the settings is not checked here: that takes RenoDX's sources and the Slang compiler
(tests/test_renodx_shaders.py compiles them where a build's folder of them is given).

    python -I -B tests/test_renodx_rows.py [--cxx g++]
"""
import argparse
import re
import struct
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]

# (the row's name, its field of Options, lowest, highest, step, the function that words its value, its default, the
# tonemapper it shows with: None any, 1 PsychoV35, 2 RenoDRT). NX2navi's menu: runtime/ui/menu.cpp row_hdr
NEW_ROWS = [
# ((Triforce, options_version 7) under the RenoDX group's heading their names lost "RenoDX "; their keys stayed: ini_key)
    ("Gamma correction", "rdx_gamma", 0, 2, 1, "tx_rdx_gamma", 0, None),
    ("Match tone slope", "rdx_match_slope", 0, 1, 1, "tx_onoff", 1, 1),
    ("Tone slope", "rdx_tone_slope", 0, 100, 1, "tx_rdx_grade", 50, 1),
    ("Hue clip", "rdx_hue_clip", 0, 100, 5, "tx_percent", 50, 1),
    ("Highlight saturation", "rdx_highlight_sat", 0, 100, 1, "tx_rdx_grade", 50, 2),
    ("Blowout", "rdx_blowout", 0, 100, 1, "tx_rdx_grade", 0, 2),
    ("Flare", "rdx_flare", 0, 100, 1, "tx_rdx_grade", 0, 2),
    ("Hue correction", "rdx_hue", 0, 100, 5, "tx_percent", 100, 2),
    ("Color grading", "rdx_grade", 0, 100, 5, "tx_percent", 100, 2),
]
KEYS = {"rdx_tonemap": "renodx_tonemapper", "rdx_gamma": "renodx_gamma_correction", "rdx_match_slope": "renodx_match_tone_slope",
        "rdx_tone_slope": "renodx_tone_slope", "rdx_hue_clip": "renodx_hue_clip",
        "rdx_highlight_sat": "renodx_highlight_saturation", "rdx_blowout": "renodx_blowout", "rdx_flare": "renodx_flare",
        "rdx_hue": "renodx_hue_correction", "rdx_grade": "renodx_color_grading"}
# (the rows RenoDX HDR had before: shown with any tonemapper, as before)
OLD_FIELDS = ["rdx_tonemap", "rdx_midgray", "rdx_ui_white", "rdx_space", "rdx_exposure", "rdx_highlights", "rdx_shadows",
              "rdx_contrast", "rdx_saturation", "rdx_bloom"]
# (struct Injection's 17 floats, then struct Extra's 6)
INJECTION = ["tone_map_type", "peak_white_nits", "diffuse_white_nits", "graphics_white_nits", "tone_map_highlights",
             "tone_map_shadows", "tone_map_contrast", "tone_map_saturation", "tone_map_cone_contrast", "bloom_strength",
             "custom_hue_clip", "custom_flags", "local_exposure", "tone_map_exposure", "adaptation_target",
             "adaptation_min_stops", "adaptation_max_stops"]
EXTRA = ["gamma_correction", "highlight_saturation", "blowout", "flare", "hue_correction", "color_grade_strength"]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def block(text, at):
    """text from at to the end of the first {...} after it"""
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


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def bits(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


# ---- the rows ----
def check_rows(menu, header):
    table = menu[menu.index("Row rows[] = {"):]
    table = table[:table.index("\n};")]
    order = re.findall(r'\{(TAB_\w+), "([^"]+)", &opts\.(\w+),', table)
    names = [name for _, name, _ in order]
    # (the tonemapper row has RenoDRT)
    row = re.search(r'\{TAB_DISPLAY, "Tonemapper", &opts\.rdx_tonemap, 0, 2, 1, tx_rdx_tonemap,(.*?)\},\n', table, re.S)
    check(row and "RenoDRT" in row.group(1) and row.group(1).rstrip().endswith("nullptr, false, true"),
          "the RenoDX tonemapper row is not Vanilla, PsychoV35, RenoDRT, changing at once")
    check(re.search(r'const char\* tx_rdx_tonemap\(int v\) \{ return v == 2 \? "RenoDRT" : v \? "PsychoV35" : "Vanilla"; \}', menu),
          "tx_rdx_tonemap does not name Vanilla (0), PsychoV35 (1) and RenoDRT (2)")
    check(re.search(r'const char\* tx_rdx_gamma\(int v\) \{ return v == 1 \? "2\.2" : v == 2 \? "BT\.1886 \(2\.4\)" : "Off"; \}', menu),
          "tx_rdx_gamma does not name Off (0), 2.2 (1) and BT.1886 (2.4) (2)")
    check(re.search(r"int rdx_tonemap = 1;", header), "the tonemapper's default is no longer PsychoV35: an options file from before would change")
    first, last = names.index("Tonemapper"), names.index("Bloom")
    for name, field, lo, hi, step, text, default, _ in NEW_ROWS:
        row = re.search(r'\{TAB_DISPLAY, "%s", &opts\.%s, (-?\d+), (-?\d+), (\d+), (\w+),(.*?)\},\n' % (re.escape(name), field), table, re.S)
        check(row, "vk_menu.cpp has no Display row \"%s\" on opts.%s" % (name, field))
        check((int(row.group(1)), int(row.group(2)), int(row.group(3)), row.group(4)) == (lo, hi, step, text),
              "the %s row is %s to %s by %s through %s, not %d to %d by %d through %s" % ((name,) + row.groups()[:4] + (lo, hi, step, text)))
        check((hi - lo) % step == 0 and lo <= default <= hi and (default - lo) % step == 0, "%s: its steps miss an end or its default" % name)
        # (read each frame: no restart; and a change leaves the Quality preset alone, as RenoDX's other rows)
        check(row.group(5).rstrip().endswith("nullptr, false, true"), "the %s row takes a restart, has a hint or changes the preset" % name)
        # (beside RenoDX's other rows, not at the tab's end)
        check(first < names.index(name) < last, "the %s row is not among RenoDX's rows on the Display tab" % name)
        check(re.search(r"\bint %s = %d;" % (field, default), header), "vk_menu.h: %s is not %d out of the box" % (field, default))
    # (values of 0 to 100 are worded by functions with room for them)
    for fn in ("tx_rdx_grade", "tx_percent"):
        size = re.search(r"const char\* %s\(int v\) \{\s*static char b\[(\d+)\];" % fn, menu)
        check(size and int(size.group(1)) > len("100%"), "%s has no room for 100" % fn)
    # (kept in the options file as every row: written by its key, read back clamped to its range)
    check('if (ini_row(r)) put(ini_key(r), std::to_string(*r.value));' in definition(menu, "void save"),
          "save no longer writes every row with a value under its key")
    init = definition(menu, "void menu_init")
    check("if (!ini_row(r) || strcmp(line, ini_key(r))) continue;" in init and "std::min(std::max(v, r.lo), r.hi)" in init,
          "menu_init no longer reads every row by its key, clamped to its range")
    line = re.search(r"char line\[(\d+)\];\s*bool has_resolution", init)
    check(line and all(len(k) + len("=100\n") < int(line.group(1)) for k in KEYS.values()), "an options line of a new row does not fit menu_init's buffer")
    shown = definition(menu, "bool row_shown")
    check("(!rdx_row(rows[i].value) || (opts.hdr == 2 && renodx_built() && opts.renodx && rdx_applies(rows[i].value)))" in shown,
          "row_shown no longer shows RenoDX's rows with HDR Native, RenoDX on and, of one tonemapper's rows, those of the one chosen")
    return [field for tab, _, field in order if field.startswith("rdx_")], [field for _, _, field in order]


MENU_FIXTURE = r"""
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
@OPTIONS@
Options opts;
struct Row { const char* name; int* value; };
@INI_KEY@
@RDX_ROW@
@RDX_APPLIES@
struct Want { const char* name; int* value; const char* key; int tonemap; int def; };
int main() {
    const Want news[] = {@NEWS@};
    int* const olds[] = {@OLDS@};
    int* const others[] = {&opts.hdr, &opts.hdr_white, &opts.hdr_peak, &opts.renodx, &opts.fps_cap, &opts.vsync};
    /* out of the box: each new row at the value that leaves the picture alone */
    for (const Want& w : news) CHECK(*w.value == w.def);
    CHECK(opts.rdx_tonemap == 1);
    /* the keys the rows are saved under */
    for (const Want& w : news) {
        const Row r{w.name, w.value};
        CHECK(!std::strcmp(ini_key(r), w.key));
    }
    const Row tonemapper{"Tonemapper", &opts.rdx_tonemap};
    CHECK(!std::strcmp(ini_key(tonemapper), "renodx_tonemapper"));
    /* every one of them is a RenoDX row (hidden without HDR Native and RenoDX HDR), and no other row is */
    for (const Want& w : news) CHECK(rdx_row(w.value));
    for (int* v : olds) CHECK(rdx_row(v));
    for (int* v : others) CHECK(!rdx_row(v) && rdx_applies(v));
    /* which show with which tonemapper: 0 Vanilla, 1 PsychoV35, 2 RenoDRT */
    for (opts.rdx_tonemap = 0; opts.rdx_tonemap <= 2; opts.rdx_tonemap++) {
        for (const Want& w : news) CHECK(rdx_applies(w.value) == (w.tonemap < 0 || w.tonemap == opts.rdx_tonemap));
        for (int* v : olds) CHECK(rdx_applies(v));   /* (the rows from before: whatever is chosen, as before) */
    }
    std::puts("renodx rows: all checks passed");
    return 0;
}
"""


# ---- the settings ----
SETTINGS_FIXTURE = r"""
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
@OPTIONS@
Options opts;
@INJECTION@
@EXTRA@
@PEAK@
@FILL@
/* one case a line: field=value pairs over the defaults; answers the 17 + 6 floats as their bits */
int main() {
    static_assert(sizeof(Injection) == 17 * sizeof(float) && sizeof(Extra) == 6 * sizeof(float));
    char line[512];
    while (std::fgets(line, sizeof line, stdin)) {
        opts = Options{};
        for (char* p = std::strtok(line, " \n"); p; p = std::strtok(nullptr, " \n")) {
            char* eq = std::strchr(p, '=');
            if (!eq) return 2;
            *eq = 0;
            const int v = std::atoi(eq + 1);
            if (false) {}
@SETTERS@
            else return 3;
        }
        Injection in{};
        Extra ex{};
        fill_settings(in, ex);
        uint32_t w[23];
        std::memcpy(w, &in, sizeof in);
        std::memcpy(w + 17, &ex, sizeof ex);
        for (uint32_t x : w) std::printf("%08x ", x);
        std::printf("\n");
    }
    return 0;
}
"""
SETTABLE = OLD_FIELDS + [r[1] for r in NEW_ROWS] + ["hdr_peak", "hdr_white"]


def before(o):
    """The 17 floats as gpu/vk/vk_renodx.cpp write_settings made them before these rows (commit f79f41d), written out again:
    int * 0.02f is the float product, correctly rounded"""
    midgray = 16 if o["rdx_midgray"] == 2 else 1 if o["rdx_midgray"] == 1 else 0
    flags = midgray | 2 | (min(max(o["rdx_space"], 0), 2) << 2)
    two, one = f32(0.02), f32(0.01)
    return [bits(9.0 if o["rdx_tonemap"] else 0.0), bits(float(o["hdr_peak"])), bits(float(o["hdr_white"])), bits(float(o["rdx_ui_white"])),
            bits(o["rdx_highlights"] * two), bits(o["rdx_shadows"] * two), bits(o["rdx_contrast"] * two), bits(o["rdx_saturation"] * two),
            bits(1.0), bits(o["rdx_bloom"] * two), bits(0.5), flags, bits(0.0), bits(o["rdx_exposure"] * one),
            bits(0.18), bits(-4.0), bits(2.0)]


def srgb_code(v):
    return v * 12.92 if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055


def check_settings(run, defaults):
    def ask(cases):
        text = "".join(" ".join("%s=%d" % kv for kv in c.items()) + "\n" for c in cases)
        out = run(text).split("\n")
        check(len(out) == len(cases) + 1, "the settings fixture answered %d lines for %d cases" % (len(out) - 1, len(cases)))
        return [[int(x, 16) for x in l.split()] for l in out[:-1]]

    def fl(word):
        return struct.unpack("<f", struct.pack("<I", word))[0]

    # 1. with the new rows out of the box, Vanilla's and PsychoV35's settings are what they were, bit for bit: over every
    # value of the rows from before that changes a field, and the HDR rows' ends
    old = []
    for tonemap in (0, 1):
        old.append({"rdx_tonemap": tonemap})
        for field, values in (("rdx_midgray", (0, 1, 2)), ("rdx_space", (0, 1, 2)), ("rdx_ui_white", (80, 200, 400)),
                              ("rdx_exposure", range(0, 201, 5)), ("rdx_highlights", range(0, 101, 5)), ("rdx_shadows", range(0, 101, 5)),
                              ("rdx_contrast", range(0, 101, 5)), ("rdx_saturation", range(0, 101, 5)), ("rdx_bloom", range(0, 101, 5)),
                              ("hdr_peak", (400, 1000, 4000, 10000)), ("hdr_white", (80, 200, 400))):
            old += [{"rdx_tonemap": tonemap, field: v} for v in values]
        old.append({"rdx_tonemap": tonemap, "rdx_midgray": 2, "rdx_space": 2, "rdx_exposure": 135, "rdx_highlights": 65, "rdx_shadows": 35,
                    "rdx_contrast": 55, "rdx_saturation": 45, "rdx_bloom": 80, "hdr_peak": 1500, "hdr_white": 250, "rdx_ui_white": 150})
    neutral = [bits(v) for v in (0.0, 1.0, 0.0, 0.0, 1.0, 1.0)]
    for case, got in zip(old, ask(old)):
        want = before(dict(defaults, **case))
        for name, a, b in zip(INJECTION, got[:17], want):
            check(a == b, "%s with %s: %s is %08x (%r), and was %08x (%r) before the rows" % (
                "PsychoV35" if case["rdx_tonemap"] else "Vanilla", case, name, a, fl(a), b, fl(b)))
        check(got[17:] == neutral, "%s: the glue's settings are not neutral out of the box (%s)" % (case, [fl(w) for w in got[17:]]))

    # 2. each new row against the one field it sets; everything else stays what it is without it
    def differs(case, fields):
        got, base = ask([case, {k: v for k, v in case.items() if k in ("rdx_tonemap",)}])
        changed = {(INJECTION + EXTRA)[i] for i in range(23) if got[i] != base[i]}
        check(changed <= set(fields), "%s changes %s, not only %s" % (case, sorted(changed), sorted(fields)))
        return {name: fl(got[i]) for i, name in enumerate(INJECTION + EXTRA)}, got

    def near(a, b):
        return abs(a - b) <= 1e-6 * max(1.0, abs(b))

    for tonemap in (0, 1, 2):
        for v in range(0, 101):
            for field, target, scale in (("rdx_tone_slope", "tone_map_cone_contrast", 50.0), ("rdx_hue_clip", "custom_hue_clip", 100.0),
                                         ("rdx_highlight_sat", "highlight_saturation", 50.0), ("rdx_blowout", "blowout", 100.0),
                                         ("rdx_flare", "flare", 50.0), ("rdx_hue", "hue_correction", 100.0),
                                         ("rdx_grade", "color_grade_strength", 100.0)):
                if v % 5 and field in ("rdx_hue_clip", "rdx_hue", "rdx_grade") or v % 7 and tonemap != 1:
                    continue   # (the rows' own steps; every value with one tonemapper, a few with the others)
                values, _ = differs({"rdx_tonemap": tonemap, field: v}, {target})
                check(near(values[target], v / scale), "%s=%d: %s is %r, not %r" % (field, v, target, values[target], v / scale))
                if v in (int(scale), int(scale) // 2, 0):   # (the values the shaders compare for: exactly 1, 0.5 or 0)
                    check(values[target] == v / scale, "%s=%d: %s is %r, not exactly %r" % (field, v, target, values[target], v / scale))
        # (match tone slope: bit 1 of the flags, RENODX_VANILLA_SLOPE)
        _, on = differs({"rdx_tonemap": tonemap, "rdx_match_slope": 1}, set())
        _, off = differs({"rdx_tonemap": tonemap, "rdx_match_slope": 0}, {"custom_flags"})
        check(on[11] & 2 and off[11] == on[11] & ~2, "Match tone slope does not set and clear bit 1 of the flags alone")
        # (the gamma correction: the glue's number for it, and the peak the tone mappers are given)
        for gamma, power in ((1, 2.2), (2, 2.4)):
            for peak, white, ui in ((1000, 200, 200), (400, 400, 400), (10000, 80, 80), (10000, 200, 400), (400, 80, 400), (4000, 300, 150)):
                case = {"rdx_tonemap": tonemap, "rdx_gamma": gamma, "hdr_peak": peak, "hdr_white": white, "rdx_ui_white": ui}
                (got, plain) = ask([case, dict(case, rdx_gamma=0)])
                check([i for i in range(23) if got[i] != plain[i]] in ([1, 17], [17]), "%s changes more than the peak and the glue's gamma" % case)
                check(fl(got[17]) == float(gamma) and fl(plain[17]) == 0.0 and fl(plain[1]) == float(peak), "%s: the glue's gamma or the plain peak" % case)
                # (the encoder shows the picture, the menus' white at 1, as a display of that power shows its sRGB code:
                # what the tone mappers were told is the peak must come out at the peak chosen)
                given = fl(got[1])
                shown = srgb_code(given / ui) ** power * ui
                check(abs(shown - peak) < 1e-4 * peak, "%s: the tone mappers are given %r nits, which the correction shows at %r, not %d" % (
                    case, given, shown, peak))
                # (above white a 2.2 display is darker than the sRGB curve and a 2.4 one brighter: the peak given is over
                # the peak chosen with the one and under it with the other, and the peak itself where the peak is white)
                check(given == peak if peak == ui else given > peak if gamma == 1 else given < peak,
                      "%s: the tone mappers are given %r nits for a peak of %d" % (case, given, peak))
    # 3. the tonemapper: RenoDX's numbers, and what follows it
    for tonemap, number, local in ((0, 0.0, 0.0), (1, 9.0, 0.0), (2, 3.0, 1.0)):
        got = ask([{"rdx_tonemap": tonemap}])[0]
        check(fl(got[0]) == number, "tonemapper %d is given to the shaders as %r, not %r" % (tonemap, fl(got[0]), number))
        check(fl(got[12]) == local, "tonemapper %d: local exposure %r, not %r" % (tonemap, fl(got[12]), local))
    # (RenoDRT differs from PsychoV35 in the tone mapper's number and the local exposure alone: the grading rows, the whites
    # and the peak reach it as they reach PsychoV35)
    mix = {"rdx_exposure": 135, "rdx_highlights": 65, "rdx_shadows": 35, "rdx_contrast": 55, "rdx_saturation": 45, "rdx_bloom": 80,
           "hdr_peak": 1500, "hdr_white": 250, "rdx_ui_white": 150, "rdx_highlight_sat": 70, "rdx_blowout": 20, "rdx_flare": 10,
           "rdx_hue": 60, "rdx_grade": 80, "rdx_gamma": 1}
    psycho, drt = ask([dict(mix, rdx_tonemap=1), dict(mix, rdx_tonemap=2)])
    check([i for i in range(23) if psycho[i] != drt[i]] == [0, 12], "RenoDRT's settings differ from PsychoV35's in more than the tone mapper and the local exposure")
    check([fl(w) for w in drt[17:]] == [1.0, f32(70 / 50.0), f32(0.2), f32(0.2), f32(0.6), f32(0.8)], "RenoDRT's rows as the glue is given them: %s" % [fl(w) for w in drt[17:]])


def check_binding(source):
    """The block the glue declares is the one vk_renodx.cpp fills, in the same order, at the binding it binds"""
    glue = (ROOT / "gpu/vk/shaders/renodx_glue/triforce_settings.slangi").read_text(encoding="utf-8")
    fields = re.search(r"struct TriforceSettings \{(.*?)\n\};", glue, re.S)
    check(fields and re.findall(r"^\s*float (\w+);", fields.group(1), re.M) == EXTRA, "triforce_settings.slangi's TriforceSettings is not the six floats of Extra, in order")
    extra = re.search(r"struct Extra \{\s*float ([^;]+);\s*\};", source)
    check(extra and [n.strip() for n in extra.group(1).split(",")] == EXTRA, "vk_renodx.cpp's Extra is not those six floats, in order")
    inject = re.search(r"struct Injection \{(.*?)\n\};", source, re.S)
    check(inject and [n.strip() for part in re.findall(r"float ([^;]+);", inject.group(1)) for n in part.split(",")] == INJECTION,
          "vk_renodx.cpp's Injection is no longer RenoDX's 17 floats in the order this check takes them in")
    check(re.search(r"layout\(set = 1, binding = 5, scalar\) uniform TriforceShaderConstants \{\s*TriforceSettings settings;\s*\} triforce_constants;", glue),
          "the glue's settings are not a scalar block at set 1, binding 5")
    check("{5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr}" in source and "li.bindingCount = 6, li.pBindings = bindings;" in source,
          "RenoDX's descriptor set has no binding 5 for the glue's settings")
    # (the set's uniform buffers in binding order from 2: the fourth is binding 5)
    buffers = re.search(r"const VkDescriptorBufferInfo buffers\[4\] = \{(.*?)\};", source, re.S)
    check(buffers and re.findall(r"\{st\.buffer, (\w+)", buffers.group(1))[2:] == ["kInjectionAt", "kExtraAt"] and
          "{st.buffer, kExtraAt, sizeof(Extra)}" in buffers.group(1) and "pBufferInfo = &buffers[i - 2]" in source and
          "vkUpdateDescriptorSets(dev.device, 6, w, 0, nullptr);" in source and "{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4 * kSets}" in source,
          "binding 5 is not Extra's place in the buffer, or the pool has no room for it")
    # (its place in the buffer is its own: a slot between the seed's and the adaptation's)
    places = re.search(r"constexpr uint64_t kInjectionAt = 0, kAaAt = kSlot, kSeedAt = 2 \* kSlot, kExtraAt = 3 \* kSlot, kAdaptationAt = 4 \* kSlot;", source)
    slot = re.search(r"constexpr uint64_t kSlot = (\d+);", source)
    check(places and slot and int(slot.group(1)) >= 17 * 4, "Extra's place in the buffer is not a slot of its own")
    write = definition(source, "void write_settings")
    check("fill_settings(in, ex);" in write and "std::memcpy(m + kInjectionAt, &in, sizeof in);" in write and
          "std::memcpy(m + kExtraAt, &ex, sizeof ex);" in write, "write_settings does not write what fill_settings made to the two blocks")
    # (the eye adaptation is PsychoV35's: with RenoDRT no draw is told to meter)
    check("bool adapting() { return opts.rdx_tonemap == 1 && opts.rdx_midgray == 2; }" in source, "the eye adaptation runs with another tonemapper than PsychoV35")
    # (shaders compiled without the glue, a header from before it: said in the log)
    check(re.search(r"#ifndef RENODX_SPV_GLUE\s.*?logf\(", source, re.S), "vk_renodx.cpp does not say when its shaders are from before the glue")
    scene = (ROOT / "gpu/vk/shaders/renodx_glue/triforce_scene.slangi").read_text(encoding="utf-8")
    check(re.search(r'#define ApplySceneToneMap RenoDXApplySceneToneMap\s+#include "scene_tonemap\.slangi"[^\n]*\s+#undef ApplySceneToneMap', scene) and
          "if (RENODX_TONE_MAP_TYPE == renodx::draw::TONE_MAP_TYPE_RENO_DRT)" in scene and
          "return RenoDXApplySceneToneMap(custom_untonemapped, custom_native_gain);" in scene,
          "the scene glue no longer hands every tonemapper but RenoDRT to RenoDX's own mapper as it is")
    # (RenoDRT starts from what the game's curve takes: the scene times the game's one exposure number, the same for all
    # three channels, which is the first entry of the gain RenoDX's shaders pass)
    check("return TriforceRenoDRT(custom_untonemapped * custom_native_gain.x, renodx::draw::BuildConfig()," in scene,
          "RenoDRT is no longer given the scene times the game's exposure")
    out = (ROOT / "gpu/vk/shaders/renodx_glue/triforce_output.slangi").read_text(encoding="utf-8")
    correct = definition(out, "float3 TriforceGammaCorrect")
    check("if (gamma_correction == 1.f) return renodx::color::correct::GammaSafe(color, false, 2.2f);" in correct and
          "if (gamma_correction == 2.f) return renodx::color::correct::GammaSafe(color, false, 2.4f);" in correct and
          correct.rstrip("}").rstrip().endswith("return color;"), "the encoder's glue does not leave the picture as it is with the gamma correction off")


def check_docs():
    readme = (ROOT / "readme.md").read_text(encoding="utf-8")
    # ((Triforce, options_version 7) RenoDX is the HDR row's Native + RenoDX, and its rows a group of their own)
    row = re.search(r"^\| RenoDX \|(.*)\|$", readme, re.M)
    check(row, "readme.md's settings table has no RenoDX row")
    check("Native + RenoDX" in readme[readme.index("| HDR |"):readme.index("| RenoDX |")], "readme.md's HDR row has no Native + RenoDX")
    for word in ("Vanilla", "PsychoV35", "RenoDRT", "Gamma correction", "Tone slope", "Hue clip", "Highlight saturation", "Blowout", "Flare",
                 "Hue correction", "Color grading"):
        check(word in row.group(1), "readme.md's RenoDX row does not name: " + word)
    guide = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    for line in ("| `renodx_tonemapper` | 0 Vanilla, 1 PsychoV35, 2 RenoDRT |",
                 "| `renodx_gamma_correction` | 0 Off, 1 2.2, 2 BT.1886 (2.4) |"):
        check(line in guide, "docs/development.md's options table does not have: " + line)
    for key in KEYS.values():
        check("`%s`" % key in guide, "docs/development.md's options table does not name " + key)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    vk = ROOT / "gpu" / "vk"
    menu = (vk / "vk_menu.cpp").read_text(encoding="utf-8")
    header = (vk / "vk_menu.h").read_text(encoding="utf-8")
    source = (vk / "vk_renodx.cpp").read_text(encoding="utf-8")
    rdx_fields, all_fields = check_rows(menu, header)
    check(set(rdx_fields) == set(OLD_FIELDS) | {r[1] for r in NEW_ROWS}, "RenoDX's rows are not the ten from before and the nine new ones: %s" % sorted(rdx_fields))
    check(len(set(KEYS.values())) == len(KEYS), "two rows share a key")
    check_binding(source)
    check_docs()
    options = re.search(r"struct Options \{.*?\n\};", header, re.S).group(0)
    defaults = {f: int(re.search(r"\bint %s = (-?\d+)[;,]" % f, options).group(1)) for f in SETTABLE if f not in
                ("rdx_shadows", "rdx_contrast", "rdx_saturation")}
    defaults.update(rdx_shadows=50, rdx_contrast=50, rdx_saturation=50)   # (declared on rdx_highlights' line)
    check("int rdx_highlights = 50, rdx_shadows = 50, rdx_contrast = 50, rdx_saturation = 50;" in options, "vk_menu.h: the grading rows' defaults moved")
    with tempfile.TemporaryDirectory(prefix="renodx-rows-") as tmp:
        work = Path(tmp)
        flags = [args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror"]
        fixture = MENU_FIXTURE
        for mark, text in (("@OPTIONS@", options), ("@INI_KEY@", definition(menu, "const char* ini_key")),
                           ("@RDX_ROW@", definition(menu, "bool rdx_row")), ("@RDX_APPLIES@", definition(menu, "bool rdx_applies")),
                           ("@NEWS@", ", ".join('{"%s", &opts.%s, "%s", %d, %d}' % (name, field, KEYS[field], -1 if tm is None else tm, default)
                                                for name, field, _, _, _, _, default, tm in NEW_ROWS)),
                           ("@OLDS@", ", ".join("&opts.%s" % f for f in OLD_FIELDS))):
            fixture = fixture.replace(mark, text)
        src, exe = work / "menu.cpp", work / "menu.exe"
        src.write_text(fixture, encoding="utf-8")
        subprocess.run(flags + [str(src), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        fixture = SETTINGS_FIXTURE
        for mark, text in (("@OPTIONS@", options), ("@INJECTION@", re.search(r"struct Injection \{.*?\n\};", source, re.S).group(0)),
                           ("@EXTRA@", re.search(r"struct Extra \{.*?\n\};", source, re.S).group(0)),
                           ("@PEAK@", definition(source, "float peak_before_gamma")), ("@FILL@", definition(source, "void fill_settings")),
                           ("@SETTERS@", "\n".join('            else if (!std::strcmp(p, "%s")) opts.%s = v;' % (f, f) for f in SETTABLE))):
            fixture = fixture.replace(mark, text)
        src, exe = work / "settings.cpp", work / "settings.exe"
        src.write_text(fixture, encoding="utf-8")
        subprocess.run(flags + [str(src), "-o", str(exe)], check=True)
        check_settings(lambda text: subprocess.run([str(exe)], input=text.encode(), stdout=subprocess.PIPE, check=True).stdout.decode().replace("\r", ""),
                       defaults)
    print("PASS: RenoDX's rows for RenoDRT and NX2navi's HDR settings, and the settings the shaders are given")


if __name__ == "__main__":
    main()
