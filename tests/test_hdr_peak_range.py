"""The HDR peak brightness row goes up to 10,000 nits, HDR10's own ceiling (it stopped at 4,000), and everything that reads
the peak takes that much: the HDR10 pass's push constant (gpu/vk/vk_framegen.cpp), the peak the game is told with HDR Native
(the renderer's answer, and the 16 bits nn::vi gives it in host/nn_vi.c), RenoDX's tone mapping, and the pass's own curve
(gpu/vk/shaders/hdr_encode.comp, whose math is checked here in Python as tests/test_hdr_encode.py does for 1,000).

    python -I -B tests/test_hdr_peak_range.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PEAK = 10000


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def pq(nits):
    m1, m2, c1, c2, c3 = 0.1593017578125, 78.84375, 0.8359375, 18.8515625, 18.6875
    l = (min(max(nits / 10000.0, 0.0), 1.0)) ** m1
    return ((c1 + c2 * l) / (1 + c3 * l)) ** m2


def nits(y, white, peak, knee=0.6):
    """hdr_encode.comp: the picture's white at the paper white, its highlights towards the peak"""
    over = min(max((y - knee) / (1 - knee), 0.0), 1.0)
    return y * white * (1 + (peak / white - 1) * over * over)


def main():
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    row = re.search(r'\{TAB_DISPLAY, "HDR peak brightness", &opts\.hdr_peak, (\d+), (\d+), (\d+), tx_nits,', menu)
    check(row, "vk_menu.cpp: the HDR peak brightness row is missing")
    lo, hi, step = (int(v) for v in row.groups())
    check((lo, hi) == (400, PEAK), "the HDR peak brightness row is %d to %d nits, not 400 to 10,000" % (lo, hi))
    check((hi - lo) % step == 0 and step == 100, "the row's steps do not reach its ends, or are not the 100 nits they were")
    # the value's text has room for five digits
    text = re.search(r"const char\* tx_nits\(int v\) \{\s*static char b\[(\d+)\];", menu)
    check(text and int(text.group(1)) > len("10000 nits"), "tx_nits has no room for 10000 nits")
    # the default stays what it was: a file from before, and a first start, run as before
    header = (ROOT / "gpu/vk/vk_menu.h").read_text(encoding="utf-8")
    check(re.search(r"int hdr_peak = 1000;", header), "the HDR peak's default is no longer 1,000 nits")

    # every reader of the peak: none stops under the row's top
    readers = {}
    for path in sorted((ROOT / "gpu/vk").glob("*.cpp")) + sorted((ROOT / "gpu/vk").glob("*.h")) + sorted((ROOT / "host").glob("*.c")):
        source = path.read_text(encoding="utf-8", errors="replace")
        for line in source.splitlines():
            if re.search(r"\bopts\.hdr_peak\b|HOS_OPT_HDR_PEAK\b", line) and "enum" not in line:
                readers.setdefault(path.name, []).append(line.strip())
    # (linux fork: vk_ffx_fg.cpp, AMD's frame generation swapchain's HDR metadata)
    check(set(readers) == {"vk_framegen.cpp", "vk_main.cpp", "vk_menu.cpp", "vk_renodx.cpp", "vk_replay.cpp", "nn_vi.c", "vk_ffx_fg.cpp"},
          "the HDR peak is read somewhere this check does not know: %s" % sorted(readers))
    for name, lines in readers.items():
        for line in lines:
            for bound in re.findall(r"std::clamp\(opts\.hdr_peak, (\d+), (\d+)\)", line):
                check(int(bound[0]) <= lo and int(bound[1]) >= PEAK, "%s clamps the HDR peak to %s..%s: %s" % (name, bound[0], bound[1], line))
            check(not re.search(r"std::min\(opts\.hdr_peak|std::min<[^>]*>\(opts\.hdr_peak", line), "%s caps the HDR peak: %s" % (name, line))
    # the HDR10 pass and the peak the game is told (HDR Native): both take it as chosen, up to 10,000
    check(any("std::clamp(opts.hdr_peak, 400, 10000)" in l for l in readers["vk_framegen.cpp"]), "the HDR10 pass no longer takes the peak up to 10,000")
    check(any("if (id == HOS_OPT_HDR_PEAK) return std::clamp(opts.hdr_peak, 400, 10000);" in l for l in readers["vk_replay.cpp"]),
          "the renderer no longer tells the host the peak up to 10,000")
    # (nn::vi's QueryDisplayHdrStatus gives the game the peak in 16 bits: 10,000 fits)
    check(any("(uint16_t)rx_option(HOS_OPT_HDR_PEAK, 1000)" in l for l in readers["nn_vi.c"]) and PEAK <= 0xFFFF,
          "host/nn_vi.c no longer hands the game the peak as it is")
    # (RenoDX's tone mapping is given the peak as it is; its shaders take peak / paper white, with no ceiling of their own)
    check(any("in.peak_white_nits = (float)opts.hdr_peak;" in l for l in readers["vk_renodx.cpp"]), "RenoDX is no longer given the peak as chosen")

    # the pass's curve at the top of the row: PQ is exactly 1 at 10,000 nits, the picture's white reaches the peak with any
    # paper white of its row (80 to 400), black stays black, the rest of the picture stays at the paper white, and brighter
    # in is never darker out
    shader = (ROOT / "gpu/vk/shaders/hdr_encode.comp").read_text(encoding="utf-8")
    check("clamp(nits / 10000.0, 0.0, 1.0)" in shader and "(pc.peak / pc.white - 1.0) * over * over" in shader,
          "hdr_encode.comp's curve is not the one checked here")
    check(abs(pq(PEAK) - 1.0) < 1e-12 and pq(PEAK + 5000) == pq(PEAK) and pq(4000) < pq(PEAK), "PQ at 10,000 nits")
    for white in (80, 200, 400):
        for peak in (400, 1000, 4000, 4100, 7500, PEAK):
            check(abs(nits(1.0, white, peak) - peak) < 1e-6, "white %d, peak %d: the picture's white is not at the peak" % (white, peak))
            check(nits(0.0, white, peak) == 0 and abs(nits(0.6, white, peak) - 0.6 * white) < 1e-9, "white %d, peak %d: under the knee" % (white, peak))
            ys = [nits(i / 1000, white, peak) for i in range(1001)]
            check(all(a <= b for a, b in zip(ys, ys[1:])), "white %d, peak %d: not monotonic" % (white, peak))
            codes = [pq(v) for v in ys]
            check(all(a <= b for a, b in zip(codes, codes[1:])) and codes[-1] <= 1.0, "white %d, peak %d: the PQ codes" % (white, peak))
    readme = (ROOT / "readme.md").read_text(encoding="utf-8")
    check("peak brightness 400 to 10,000 nits" in readme, "readme.md's settings table does not say the HDR peak's range")
    guide = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    check("| `hdr_paper_white`, `hdr_peak_brightness` | nits: 80 to 400, and 400 to 10,000 |" in guide,
          "docs/development.md's options table does not say the HDR rows' ranges")
    print("PASS: the HDR peak brightness row reaches 10,000 nits, and so does everything that reads it")


if __name__ == "__main__":
    main()
