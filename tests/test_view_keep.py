"""Texture looks kept through other textures' ends (gpu/vk/view_keep.h, gpu/vk/texture_gone.h, vk_draw.cpp texture_view_of,
vk_texture.cpp destroy_later / bc1_retype / compact_step; no game, no GPU).

A recording thread keeps what each texture handle resolved to, and any texture's end (an eviction, a dead render target let go,
a texture made BC3 from BC1, the compaction moving one) made every look of every thread stale: each handle of each draw was then
resolved anew. Now the textures whose views went are recorded by their creation number before the generation moves, and a
thread that finds it moved keeps the looks of the others (after NX2navi's tex_gen_undo; rewritten for this renderer's lookups).

Compiles and runs the header against a model of a thread's looks (tests/view_keep_test.cpp): a look taken again gives what
resolving it anew would, and looks of textures not gone are not resolved anew; then each of the rule's parts is taken away in
turn (a mutation of the header) and the model must fail. Reads the source for what needs the game: every place a texture's views
go records it after they went and before the generation moves, every move of the generation is such a place or one that only
looks of no texture depend on, texture_view_of passes before it reads a look and keeps only looks of a texture's own image, the
switch in the development guide; and the source check itself is mutation-checked.

    python -I -B tests/test_view_keep.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def block(text, head):
    at = text.index(head)
    end = text.index("{", at) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[at:end]


def need(where, text, needles):
    for needle in needles:
        if needle not in text:
            raise SystemExit(where + " lacks: " + needle)


def in_order(where, text, needles):
    at = -1
    for needle in needles:
        k = text.find(needle, at + 1)
        if k < 0:
            raise SystemExit(where + ": not in this order (%s)" % " < ".join(needles))
        at = k


def check_source(tex, draw, header, dev):
    # (each place a texture's views go: they go, the number is recorded, then the generation moves)
    in_order("vk_texture.cpp destroy_later", block(tex, "void destroy_later(Texture& tex) {"),
             ["gone_record.went(tex.id);", "d.view_map = std::move(tex.views)", "d.views.push_back(kv.second)", "dead.push_back(std::move(d));",
              "tex.twin3d_views.clear();", "views_gone.went(tex.id);", "destroy_gen++;"])
    in_order("vk_texture.cpp bc1_retype", block(tex, "bool bc1_retype(Texture& x, const char* why) {"),
             ["x.views.clear();", "x.image = image, x.allocation = allocation;", "x.bc1 = false, x.bc1_no = true;", "views_gone.went(x.id);",
              "destroy_gen++;"])
    compact = block(tex, "void compact_step(uint64_t completed) {")
    in_order("vk_texture.cpp compact_step", compact,
             ["t->views.clear();", "t->image = image;", "views_gone.went(t->id);",
              "destroy_gen++;   // (what was resolved to the moved textures' views is stale)"])
    # (no other move of the generation than those and the frame's for the dummy's looks, which are looks of no texture)
    functions = {"destroy_later": block(tex, "void destroy_later(Texture& tex) {"),
                 "bc1_retype": block(tex, "bool bc1_retype(Texture& x, const char* why) {"), "compact_step": compact,
                 "texture_frame": block(tex, "void texture_frame(uint64_t completed) {")}
    if tex.count("destroy_gen++") != sum(f.count("destroy_gen++") for f in functions.values()):
        raise SystemExit("vk_texture.cpp moves destroy_gen in a function the views' record does not know (view_keep.h)")
    if functions["texture_frame"].count("destroy_gen++") != 2 or functions["texture_frame"].count("else destroy_gen++;") != 2:
        raise SystemExit("vk_texture.cpp texture_frame moves destroy_gen otherwise than for the dummy's looks")
    # (no other place lets a live texture's views go)
    for needle in ("views.clear()", "std::move(tex.views)", "std::move(t->views)", "std::move(x.views)"):
        total = tex.count(needle)
        known = sum(f.count(needle) for k, f in functions.items() if k != "texture_frame")
        if total != known:
            raise SystemExit("vk_texture.cpp lets a texture's views go (%s) outside destroy_later, bc1_retype and compact_step" % needle)
    need("vk_texture.cpp", tex, ["texture_gone::Record views_gone;",
                                 "return views_gone.went_since(since, out, now);"])
    need("vk_texture.h", header, ["bool texture_views_gone_since(uint64_t since, std::vector<uint32_t>& out, uint64_t* now);"])
    # (the thread passes with the generation read first, before it reads a look; only a texture's own image's view is stable)
    view = block(draw, "VkImageView texture_view_of(")
    in_order("vk_draw.cpp texture_view_of", view,
             ["const uint64_t dgen = texture_destroy_generation();", "view_keep::pass(view_pass, dgen,",
              "fn(kv.second.destroy_gen, kv.second.id, kv.second.stable)", "texture_views_gone_since, view_gone()",
              "handle_hits().find(key)", "it->second.destroy_gen == dgen",
              "const bool stable = view && tex && !(want == VK_IMAGE_VIEW_TYPE_3D && tex->type != VK_IMAGE_TYPE_3D);",
              "tex ? tex->id : 0u, stable};"])
    need("vk_draw.cpp", draw, ['#include "view_keep.h"', "thread_local view_keep::Pass view_pass;",
                               "view_keep::wanted(getenv(\"RX_VK_VIEW_KEEP\"))"])
    need("docs/development.md", dev, ["`RX_VK_VIEW_KEEP=0`"])


def source_mutations(tex, draw, header, dev):
    """The source check fails on each of these (the check is what guards the record's order)"""
    cases = [
        ("destroy_later records before its views went",
         lambda t, d: (t.replace("    gone_record.went(tex.id);\n", "    gone_record.went(tex.id);\n    views_gone.went(tex.id);\n", 1)
                       .replace("    views_gone.went(tex.id);   // ((Triforce) its views went: noted, then the generation moves (view_keep.h))\n", "", 1), d)),
        ("destroy_later moves the generation first",
         lambda t, d: (t.replace("    gone_record.went(tex.id);\n", "    destroy_gen++;\n    gone_record.went(tex.id);\n", 1)
                       .replace("    views_gone.went(tex.id);   // ((Triforce) its views went: noted, then the generation moves (view_keep.h))\n    destroy_gen++;\n",
                                "    views_gone.went(tex.id);   // ((Triforce) its views went: noted, then the generation moves (view_keep.h))\n", 1), d)),
        ("bc1_retype records nothing",
         lambda t, d: (t.replace("    views_gone.went(x.id);   // ((Triforce) its views went: noted, then the generation moves (view_keep.h))\n", "", 1), d)),
        ("the compaction records nothing",
         lambda t, d: (t.replace("        views_gone.went(t->id);   // ((Triforce) its views went; the generation moves after the loop (view_keep.h))\n", "", 1), d)),
        ("a new place lets views go",
         lambda t, d: (t.replace("void texture_released(const nvn_tex* t) {\n", "void texture_released(const nvn_tex* t) {\n    by_gpu.begin()->second->views.clear();\n", 1), d)),
        ("a new move of the generation",
         lambda t, d: (t.replace("void texture_released(const nvn_tex* t) {\n", "void texture_released(const nvn_tex* t) {\n    destroy_gen++;\n", 1), d)),
        ("a 3D copy's look counted stable",
         lambda t, d: (t, d.replace(" && !(want == VK_IMAGE_VIEW_TYPE_3D && tex->type != VK_IMAGE_TYPE_3D);", ";", 1))),
        ("the pass after the look is read",
         lambda t, d: (t, d.replace("    const auto it = handle_hits().find(key);\n", "", 1)
                       .replace("    static const bool log_hits = getenv(\"RX_VK_LOG_HITS\") != nullptr;\n",
                                "    static const bool log_hits = getenv(\"RX_VK_LOG_HITS\") != nullptr;\n", 1)
                       .replace("    const uint64_t dgen = texture_destroy_generation();\n",
                                "    const uint64_t dgen = texture_destroy_generation();\n    const auto it = handle_hits().find(key);\n", 1))),
    ]
    for what, mutate in cases:
        t, d = mutate(tex, draw)
        if (t, d) == (tex, draw):
            raise SystemExit("source mutation %r changed nothing: its text is gone" % what)
        try:
            check_source(t, d, header, dev)
        except SystemExit:
            print("PASS: the source check fails when %s" % what)
            continue
        raise SystemExit("the source check passes when %s" % what)


HEADER_MUTATIONS = [
    ("view_keep.h", "return told && gen == last && stable && id &&", "return gen == last && stable && id &&",
     "a look stands when the textures gone could not all be told"),
    ("view_keep.h", "return told && gen == last && stable && id &&", "return told && stable && id &&",
     "a look stale since an earlier pass stands"),
    ("view_keep.h", "return told && gen == last && stable && id &&", "return told && gen == last && id &&",
     "a look of no view or of a 3D copy stands"),
    ("view_keep.h", "return told && gen == last && stable && id &&", "return told && gen == last && stable &&",
     "a look of no texture stands"),
    ("view_keep.h", "&& !std::binary_search(gone.begin(), gone.end(), id);", ";",
     "a look of a texture whose views went stands"),
    ("view_keep.h", "    std::sort(gone.begin(), gone.end());\n", "",
     "the numbers gone are searched unsorted"),
    ("view_keep.h", "inline bool wanted(const char* env) { return !(env && env[0] == '0'); }",
     "inline bool wanted(const char* env) { return env && env[0] == '1'; }", "the rule off unless asked"),
    ("view_keep.h", "if (stands(g, id, stable, last, gone, told)) g = gen, moved++;",
     "if (false && stands(g, id, stable, last, gone, told)) g = gen, moved++;", "no look kept: every one resolved anew"),
    ("texture_gone.h", "if (since > n_ || n_ - since > kKept) return false;", "if (since > n_) return false;",
     "the record tells numbers it no longer keeps"),
]


def build(cxx, tmp, name, headers):
    d = Path(tmp) / name
    d.mkdir()
    for file, text in headers.items():
        (d / file).write_text(text, encoding="utf-8")
    exe = d / "view-keep"
    subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-I" + str(d),
                    str(ROOT / "tests/view_keep_test.cpp"), "-o", str(exe)], check=True)
    return exe


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    tex = (VK / "vk_texture.cpp").read_text(encoding="utf-8")
    draw = (VK / "vk_draw.cpp").read_text(encoding="utf-8")
    header = (VK / "vk_texture.h").read_text(encoding="utf-8")
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    check_source(tex, draw, header, dev)
    source_mutations(tex, draw, header, dev)
    headers = {f: (VK / f).read_text(encoding="utf-8") for f in ("view_keep.h", "texture_gone.h")}
    with tempfile.TemporaryDirectory(prefix="view-keep-") as tmp:
        exe = build(args.cxx, tmp, "base", headers)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
        if r.returncode:
            raise SystemExit("the model fails with the rule as it is:\n" + r.stdout + r.stderr)
        print(r.stdout.strip())
        for k, (file, old, new, what) in enumerate(HEADER_MUTATIONS):
            if headers[file].count(old) != 1:
                raise SystemExit("mutation %r: its text is not once in %s" % (what, file))
            exe = build(args.cxx, tmp, "m%d" % k, dict(headers, **{file: headers[file].replace(old, new)}))
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
            if r.returncode == 0 or "FAIL" not in r.stderr:
                raise SystemExit("mutation %r: the model still passes" % what)
            print("PASS: without the rule (%s) the model fails: %s" % (what, r.stderr.strip().splitlines()[0][:150]))
    print("PASS: texture looks kept through other textures' ends, and only those")


if __name__ == "__main__":
    main()
