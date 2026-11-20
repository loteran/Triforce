"""A draw whose bindings moved keeps the descriptors pushed before it whole while its signature, what each of them is resolved from, is
the same, and only where a build would have taken every one of them again (gpu/vk/desc_sig.h).

1. gpu/vk/desc_sig.h compiled alone (case header): a hit needs the bind point's signature to be armed, of the same program and equal;
   a commit makes the draw's the bind point's, forget leaves none; the head carries each kind's stamps exactly as desc_keep.h's
   stamps_of gives them.
2. vk_draw.cpp's signature block (desc_sig_mode, sig_shape, desc_sig_walk, sig_sampled, sig_check), the per-binding reuse block and
   the texture handle (handle_of, handle_is_storage_pointer, sampled_handle) cut out of the source and run against stand-ins
   (tests/desc_sig_test.cpp), with a model of build_descriptors that resolves through the cut helpers in its order:
   - coverage: every input and stamp the program's descriptors stand on stops a hit (a user uniform buffer's address and size, a
     storage buffer's address and size words, a texture's handle, its sampler, its slot registered anew, the samplers', a texture
     destroyed, one left for later, the next submission, a pool moved, an inline update, a new recording, the banks' stage
     generations), and what the program does not read does not (another stage's or index's buffers, an unsampled slot, bind_gen
     alone); a texture the GPU wrote since is read again as a build would;
   - armed: a build that dropped one (a stream copy, an unbound buffer, no buffer at all) stands for nothing; a texture destroyed
     while its lookup ran makes the next walk differ;
   - random: 20000 draws with every one of those moved at random: each hit is a build that would have resolved nothing and given
     the writes and sampled textures the bind point holds;
   - shapes: written storage buffers, tables, the miss binding, run-time pointers, texel buffers, storage images, c19 and up,
     compute, the mesh fallback, RenoDX's, a compute stage: no walk;
   - check, check-old: RX_VK_DESC_SIG=check counts and logs a build that is not what is pushed, by either bookkeeping;
   - on, sig-off, keep-off, keep-check: the switch, and off with the per-binding reuse off or checked.
3. Mutations of the cut walk (each rule taken out in turn) make a case fail: the cases stand on every rule.
4. vk_draw.cpp's draw and build_descriptors (source): the walk only for push programs outside a dump and the upscaler's second
   recording, a hit only with the last draw's program and its push state at the bind point; the build's drops and buffers decide
   whether its signature is armed; the signature is committed only after the bind, forgotten for a bind it does not stand for; the
   check before the push; the sampled textures read again on a hit; the build and the walk read handles through sampled_handle and
   the storage buffers' words alike; the walk's bank generation is uniform_range's; the shape's storage buffer rule is the build's.
python -I -B tests/test_desc_sig.py --headers <Vulkan include folder> [--cxx g++] [--source <vk_draw.cpp>] [--no-mutations]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]
CASES = ["header", "on", "sig-off", "keep-off", "keep-check", "coverage", "armed", "random", "shapes", "check", "check-old"]
MUTANT_CASES = ["coverage", "armed", "shapes", "random"]   # (what a mutation of the walk can break)
# (a rule of the walk, and what it is replaced by: each must make a case fail)
MUTATIONS = [
    ("now.epoch = texture_epoch();", "now.epoch = 0;", "the next submission's epoch"),
    ("now.destroy = texture_destroy_generation();", "now.destroy = 0;", "a texture destroyed"),
    ("now.samp = sampler_registrations_generation(),", "now.samp = 0,", "a sampler registration"),
    ("now.rec = desc_rec, ", "now.rec = 0, ", "a new recording"),
    ("now.ub = ub_generation();", "now.ub = 0;", "an inline update"),
    ("now.dummy = texture_dummy_generation(),", "now.dummy = 0,", "a texture left for later"),
    ("now.bind = gpu_bind_stamp(),", "now.bind = 0,", "a pool moved"),
    ("w.push_back(u.addr), w.push_back(u.size);", "w.push_back(u.addr);", "a user uniform buffer's size"),
    ("w.push_back(stage_cbuf_word(s, st, d.cbuf_index, off + 8));", "", "a storage buffer's size word"),
    ("(uint64_t)texture_slot_version((int)(h & 0xFFFFF)) << 32 | h", "h", "a slot registered anew"),
    ("index == 2 ? s.tex_gen[ns] : s.ssbo_gen[ns]", "s.tex_gen[ns]", "a bank's own stage generation"),
    ("d.count != 1 || d.is_written || ", "d.count != 1 || ", "a written storage buffer"),
    ("|| !info.image_descriptors.empty()", "", "a storage image"),
    ("bool ok = !p.compute && !p.fallback && !p.renodx;", "bool ok = !p.compute && !p.renodx;", "the mesh fallback"),
    ("for (const void* t : b.tex)\n        if (static_cast<const Texture*>(t)->gpu_written)", "for (const void* t : b.tex)\n        if (true)",
     "the sampled textures the GPU wrote"),
]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(cond, why, fails):
    if not cond:
        fails.append(why)


def before(text, a, b, why, fails):
    need(a in text and b in text and text.index(a) < text.index(b), why, fails)


def check_source(source):
    fails = []
    draw = block(source, "void draw(VkCommandBuffer")
    need("const bool sig_walked = sig_mode && p.push && !dump_on() && !pre_retarget_on && desc_sig_walk(s, p, sig);" in draw,
         "the walk is not only for push programs outside a dump and the upscaler's second recording", fails)
    need("const bool sig_hit = sig_walked && last_desc_valid && last_desc_prog == &p && pushed()[0].cmd == cmd &&\n"
         "                         pushed()[0].layout == p.layout && desc_sig::hit(sig_bound(), sig, &p);" in draw,
         "a hit does not need the last draw's program and its push at the bind point", fails)
    need("const int sig_mode = !same_gens && !no_reuse ? desc_sig_mode() : 0;" in draw, "the walk runs where the draw keeps them anyway",
         fails)
    need("const bool same_desc = same_gens || (sig_hit && sig_mode == 1);" in draw, "a checked hit is not built", fails)
    build = draw[draw.index("if (!same_desc) {"):]
    before(build, "const int64_t drops = desc_drops", "build_descriptors(s, p, true);", "the drops are not read before the build", fails)
    before(build, "build_descriptors(s, p, true);", "sig_armed = desc_drops == drops;", "the signature is armed before the build", fails)
    need("sig_armed &= b.buffer != VK_NULL_HANDLE;" in build, "a bank not made arms the signature", fails)
    need("sig.tex.assign(built_textures().begin(), built_textures().end());" in build, "the build's textures are not kept", fails)
    bind = draw[draw.index("FINE_SCOPE(FT_BINDDESC);"):]
    before(bind, "if (sig_hit) sig_check(cmd, p);", "if (!bind_descriptors(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p)) SKIP_HERE();",
           "the check comes after the push changed what is pushed", fails)
    before(bind, "if (!bind_descriptors(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p)) SKIP_HERE();",
           "if (sig_walked) desc_sig::commit(sig_bound(), sig, sig_armed);", "the signature is committed before the bind", fails)
    need("else desc_sig::forget(sig_bound());" in bind, "a bind no signature stands for leaves the last one", fails)
    need("else if (!same_gens) sig_sampled(sig_bound(), gfx_sampled());" in draw, "a hit does not read its sampled textures again",
         fails)
    fn = block(source, "void build_descriptors(")
    need("const uint32_t handle = sampled_handle(s, st, d, k);" in fn, "the build reads a texture's handle otherwise", fails)
    need("built_textures().clear();" in fn and "if (view && tex) built_textures().push_back(tex);" in fn,
         "the build does not say which textures it sampled", fails)
    need("const uint64_t addr = stage_cbuf_word(s, st, d.cbuf_index, off) |\n"
         "                                      (uint64_t)stage_cbuf_word(s, st, d.cbuf_index, off + 4) << 32;" in fn and
         "uint64_t size = stage_cbuf_word(s, st, d.cbuf_index, off + 8);" in fn, "the build reads a storage buffer otherwise", fails)
    need("kp && d.count == 1 && !d.is_written && !info.native_global_bounds" in fn and
         "d.count == 1 && d.cbuf_index == 0 && d.cbuf_offset == 0xff0" in fn,
         "the build keeps other storage buffers than the shape takes", fails)
    sig = source[source.index("// ---- (Triforce) The whole-draw skip by the signature"):source.index("// ---- RX_VK_CHECKPOINTS")]
    walk = block(sig, "bool desc_sig_walk(")
    need("const uint32_t h = sampled_handle(s, st, d, k);" in walk, "the walk reads a texture's handle otherwise", fails)
    ur = block(source, "VkDescriptorBufferInfo uniform_range(")
    need("const uint64_t gen = stage_gen_off || bank_stage > 4 ? s.bind_gen : index == 2 ? s.tex_gen[bank_stage] : s.ssbo_gen[bank_stage];"
         in ur and "stage_gen_off || ns > 4 ? s.bind_gen : index == 2 ? s.tex_gen[ns] : s.ssbo_gen[ns]" in walk and
         'static const bool stage_gen_off = getenv("RX_VK_BANK_GEN_OFF") != nullptr;' in ur and
         'static const bool stage_gen_off = getenv("RX_VK_BANK_GEN_OFF") != nullptr;' in walk,
         "the walk's bank generation is not uniform_range's", fails)
    keep = source[source.index("// ---- (Triforce) Per-binding descriptor reuse (desc_keep.h)"):source.index("void build_descriptors(")]
    need("else desc_keep::drop(e), desc_drops++;" in block(keep, "void buf_keep(") and
         "else drop(*e), desc_drops++;" in block(keep, "VkSampler kept_texture("), "a dropped descriptor is not counted", fails)
    mode = block(sig, "int desc_sig_mode()")
    need("desc_keep_mode() != 1" in mode, "the signature stands without the per-binding reuse", fails)
    return fails


def cut(source):
    handles = "template <class D>\n" + block(source, "uint32_t handle_of(const NvnState& s, const Stage& st, const D& d, uint32_t k)") + "\n" + \
              block(source, "bool handle_is_storage_pointer(") + "\n" + block(source, "uint32_t sampled_handle(")
    keep = source[source.index("// ---- (Triforce) Per-binding descriptor reuse (desc_keep.h)"):source.index("void build_descriptors(")]
    words = block(source, "struct Pushed {") + ";\n" + block(source, "void write_words(") + "\n" + block(source, "void words_append(")
    sig = source[source.index("// ---- (Triforce) The whole-draw skip by the signature"):source.index("// ---- RX_VK_CHECKPOINTS")]
    return handles, keep, words, sig


def build_and_run(a, tmp, name, handles, keep, words, sig, quiet=False):
    fixture = (ROOT / "tests" / "desc_sig_test.cpp").read_text(encoding="utf-8")
    for mark, text in [("/* HANDLES */", handles), ("/* KEEP */", keep), ("/* WORDS */", words), ("/* SIG */", sig)]:
        fixture = fixture.replace(mark, text)
    cpp, exe = Path(tmp) / (name + ".cpp"), Path(tmp) / (name + ".exe")
    cpp.write_text(fixture, encoding="utf-8")
    r = subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                        "-Wno-unused-function", "-pthread", "-I" + str(a.headers), "-I" + str(ROOT / "gpu" / "vk"), str(cpp), "-o", str(exe)],
                       capture_output=quiet, text=True)
    if r.returncode:
        raise SystemExit("%s does not compile%s" % (name, ":\n" + r.stderr[-3000:] if quiet else ""))
    failed = []
    for case in MUTANT_CASES if quiet else CASES:
        r = subprocess.run([str(exe), case], timeout=60, capture_output=quiet, text=True)
        if r.returncode:
            failed.append(case)
            if not quiet:
                break
    return failed


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--headers", type=Path, required=True)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--source", type=Path, default=ROOT / "gpu" / "vk" / "vk_draw.cpp", help="another vk_draw.cpp (a mutation)")
    p.add_argument("--no-mutations", action="store_true", help="the cases only")
    a = p.parse_args()
    source = a.source.read_text(encoding="utf-8")
    fails = check_source(source)
    if fails:
        raise SystemExit("\n".join(fails))
    handles, keep, words, sig = cut(source)
    with tempfile.TemporaryDirectory(prefix="desc-sig-") as tmp:
        failed = build_and_run(a, tmp, "fixture", handles, keep, words, sig)
        if failed:
            raise SystemExit("case %s failed" % failed[0])
        survived = []
        if not a.no_mutations:
            for i, (old, new, what) in enumerate(MUTATIONS):
                if sig.count(old) != 1:
                    raise SystemExit("the mutation's text is not in the walk once: %r" % old)
                if not build_and_run(a, tmp, "mutant%d" % i, handles, keep, words, sig.replace(old, new), quiet=True):
                    survived.append(what)
    if survived:
        raise SystemExit("no case fails without: " + "; ".join(survived))
    print("PASS: %d cases, %d mutations caught: a draw keeps its descriptors whole only where a build would take each of them again"
          % (len(CASES), 0 if a.no_mutations else len(MUTATIONS)))


if __name__ == "__main__":
    main()
