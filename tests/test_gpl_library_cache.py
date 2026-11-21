"""The stage libraries are kept in Hookshot's own pipeline cache, and a warm pass only counts once that cache is on disk
(gpu/vk/vk_draw.cpp build_graphics, warm_libraries; source checks, no game/GPU).

Asked for under the compile-required flag, the driver answers from a cache it is handed: a library the warm pass built into
nothing and let go was cold again at its first draw. So parts 1 and 2 are built into the cache, the cache exists before the
warm pass's workers start (the start that only warms had not made it), what is compiled counts towards a save, and the
warmed stamp is not written before the file holds the libraries.
(Triforce) One exception, on purpose and checked here: a cache that is over the file's bound with the libraries in it
(gpu/vk/pcache_bound.h) is not written at all, and the pass then counts as done with a line in the log that says so, where
it would otherwise be made again at every start and never kept. That way out stands before the save, not after it.
python -I -B tests/test_gpl_library_cache.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def between(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def main():
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    build = between(draw, "VkPrimitiveTopology topo, int part, bool* cold) {", "\n}\n")
    for line, why in (
            ("const bool lib_cached = (part == 1 || part == 2) && gpl_libs_cached();", "only the two shader libraries are kept"),
            ("const VkPipelineCache wanted = lib_cached || (part < 0 && !driver_cache_only) ? pcache : VK_NULL_HANDLE;",
             "the libraries are not built into the pipeline cache"),
            # ((Triforce) through the cache's gate, held over the creation: none once it is let go for the RAM, pcache_gate.h)
            ("const PcacheUse use(pcache_door, wanted);", "the creation does not hold the cache's gate"),
            ("const VkPipelineCache cache = use.cache;", "the creation does not take the cache the gate gives"),
            ("VkResult r = cached_graphics(cache, &ci, &pl);", "pipeline creation bypasses the chosen cache"),
            ("pcache_built_since_save += lib_cached && !cold;", "a compiled library does not count towards a save")):
        if line not in build:
            raise SystemExit(why)
    warm = between(draw, "bool warm_libraries(", "\n}\n")
    if "pcache_init();" not in warm or warm.index("pcache_init();") > warm.index("std::thread"):
        raise SystemExit("the warm pass's workers start before the pipeline cache is there")
    saved = warm.index("const bool saved = pcache_saved_now();")
    over = warm.find("!pcache_bound::within(held, pcache_bound_bytes)) {")
    if over < 0 or over > saved or "over the bound of" not in warm[over:saved] or warm[over:saved].count("return true;") != 1 or \
            "return true;" in warm[:over]:
        raise SystemExit("the warm pass is taken for done without its libraries on disk, other than for a cache over the bound "
                         "and with a log line that says so")
    # ((Triforce) over the bound with an older file's data in it (a file carried over an update): that file is not read again,
    # and the part is not taken for done, so that the next start makes a new file with only these shaders' pipelines; a new file
    # over the bound too is the way out above. Not with RX_VK_PCACHE_KEEP_OLD (it would be read again at every start), and only
    # once the .for is gone (else the same file would come back)
    # ((Triforce) nor for a file of this part's own kind, <file>.own: a checkpoint's before a crash, warm_checkpoint.h)
    carried = warm.find("if (pcache_from_file && !pcache_from_own && !getenv(\"RX_VK_PCACHE_KEEP_OLD\") && remove((pcache_path + \".for\").c_str()) == 0) {")
    if carried < over or carried > warm.index("warm_unsaved = true;", over) or \
            "warm_reset = true;\n                return false;\n            }" not in warm[carried:warm.index("warm_unsaved = true;", over)]:
        raise SystemExit("a cache over the bound only by a carried file's old pipelines is kept so for good")
    init = between(draw, "void pcache_init() {", "\n}\n")
    if "pcache_from_file = pcache && ci.initialDataSize > 0;" not in init or \
            init.index("pcache_from_file = pcache && ci.initialDataSize > 0;") < init.rindex("vkCreatePipelineCache("):
        raise SystemExit("pcache_init does not say whether the cache began from the file")
    if "if (cold && !prepare_stopped() && !warm_reset && !pcache_carry_over.load()) lib_check_made(path, warmed, false);" not in \
            between(draw, "bool prepare_run(", "\n}\n"):
        raise SystemExit("a part to be made again into a new file is recorded as a making that did not stay")
    if "if (!saved) return false;" not in warm[saved:] or warm.index("return true;", saved) < warm.index("if (!saved) return false;"):
        raise SystemExit("a warm pass whose libraries are not on disk is taken for done")
    prep = between(draw, "bool prepare_run(", "\n}\n")
    for call in ("if (!warm_libraries(progress)) {",):
        at = 0
        while (at := prep.find(call, at)) >= 0:
            if prep.index("return false;", at) > prep.index("prepared_for.warmed = warmed;", at):
                raise SystemExit("the warmed stamp is written although the warm pass did not finish")
            at += 1
    if "kWarmRecipe = 1;" in draw or "gpl_libs_cached() ? 1u : 3u" not in between(draw, "uint64_t warmed_stamp_of(", "\n}\n"):
        raise SystemExit("a folder warmed before the libraries were kept would read as warmed")
    print("PASS: the stage libraries go into the pipeline cache, and the warm pass counts once they are saved")


if __name__ == "__main__":
    main()
