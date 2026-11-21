"""Opaque ASTC textures kept as BC1 instead of BC3 (gpu/vk/tex_bc1.h, gpu/vk/astc_opaque.h, gpu/vk/vk_texture.cpp): no game,
no GPU.

What it shows:
- the picture: a BC3 block's colour half and the same eight bytes as a BC1 block decode to the same sixteen texels exactly
  when tex_bc1::colour_half_same says so, over authored blocks (the colour order that turns BC1 to its three-colour mode
  among them) and 800,000 at random; every pair of colours the encoder's ordering writes passes; an opaque block's BC3
  alpha half decodes to 255, what BC1's RGB format gives (tests/test_tex_bc1.cpp, decoders written from the Khronos Data
  Format Specification);
- the encoder: the SPIR-V in gpu/vk/host_shaders/bc_encoder_comp_spv.h is the one tex_bc1.h describes (its bytes by their
  SHA-256), and in it main calls one function for the colour block of both kinds, EncodeColor: with the constant false for
  BC3's colour half, with "a texel's alpha is under 0.5" for the block written alone. For texels that are all opaque
  those are the same call;
- which blocks are opaque: astc_opaque.h on authored ASTC blocks, each of the decoder's errors and each endpoint mode, and,
  where the CPU decoder compiles here, against the repository's CPU ASTC decoder on 840,000 blocks made at random: a
  block it passes decodes with alpha 255 in every texel. The GPU decoder itself cannot be run here: the header's rules
  were read from its SPIR-V (gpu/vk/host_shaders/astc_decoder_comp_spv.h, pinned by its SHA-256 too);
- the renderer: where vk_texture.cpp and vk_xfer.cpp use them (the image's format, the views' formats, the encoder's
  switch, the bytes counted, the check of the bytes staged, the copies), the Graphics tab's row, Off unless set, and the
  switch over it; the readme's and the development guide's rows.

    python -I -B tests/test_tex_bc1.py [--cxx g++]
"""
import argparse
import hashlib
import re
import struct
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"

# (the two shaders the headers' reasoning is about: a header made anew has to be read anew, tex_bc1.h and astc_opaque.h
# brought up to it, and its hash put here)
ENCODER_SHA256 = "22913b4b9b18e113aecd728e8523a256649aea90b5dbf24353d5df5bb89042b6"
DECODER_SHA256 = "64cee632883e52be013e23562f24d5a389055c73cc321e1413409c68a0b377d2"


def check(ok, what):
    if not ok:
        raise SystemExit("FAIL: " + what)


def words_of(header):
    return [int(w, 16) for w in re.findall(r"0x[0-9a-fA-F]{8}", header.read_text(encoding="utf-8"))]


def spirv(words):
    """(opcode, operands) of every instruction, and the names OpName gives"""
    insts, names, i = [], {}, 5
    while i < len(words):
        n, op = words[i] >> 16, words[i] & 0xFFFF
        check(n > 0, "the SPIR-V does not parse")
        insts.append((op, words[i + 1:i + n]))
        i += n
    for op, a in insts:
        if op == 5:   # OpName
            raw = b"".join(struct.pack("<I", w) for w in a[1:])
            names[a[0]] = raw[:raw.index(b"\0")].decode()
    return insts, names


def check_encoder():
    header = VK / "host_shaders" / "bc_encoder_comp_spv.h"
    digest = hashlib.sha256(header.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    check(digest == ENCODER_SHA256, "bc_encoder_comp_spv.h changed (%s): read the new shader, bring gpu/vk/tex_bc1.h up to it, "
          "then put its hash into this test" % digest)
    insts, names = spirv(words_of(header))
    by_name = {v: k for k, v in names.items()}
    colour = [k for k, v in names.items() if v.startswith("EncodeColor(")]
    alpha = [k for k, v in names.items() if v.startswith("EncodeAlpha(")]
    check(len(colour) == 1 and len(alpha) == 1 and "main" in by_name, "the encoder has no EncodeColor, EncodeAlpha or main")
    false_ids = {a[1] for op, a in insts if op == 42}   # OpConstantFalse
    # main's body: the first OpFunction is main's (glslang writes the entry point first)
    start = next(i for i, (op, a) in enumerate(insts) if op == 54 and a[1] == by_name["main"])
    end = next(i for i in range(start, len(insts)) if insts[i][0] == 56)
    body = insts[start:end]
    calls = [a for op, a in body if op == 57]   # OpFunctionCall: type, result, function, arguments
    check(sorted(c[2] for c in calls) == sorted([alpha[0], colour[0], colour[0]]),
          "the encoder's main no longer calls EncodeAlpha once and EncodeColor twice")
    last_store = {}
    flags = []
    for op, a in body:
        if op == 62:   # OpStore pointer, object
            last_store[a[0]] = a[1]
        if op == 57 and a[2] == colour[0]:
            flags.append(last_store.get(a[4]))
    loads = {a[1]: a[2] for op, a in body if op == 61}   # OpLoad: result -> pointer
    constant = [f for f in flags if f in false_ids]
    variable = [f for f in flags if f not in false_ids]
    check(len(constant) == 1 and len(variable) == 1, "EncodeColor is not called once with the constant false and once with a variable")
    check(names.get(loads.get(variable[0])) == "any_transparent", "EncodeColor's other flag is not any_transparent")
    # any_transparent starts false and is only ever "or a texel's alpha is under 0.5"
    half = {a[1] for op, a in insts if op == 43 and len(a) == 3 and a[2] == 0x3F000000}   # OpConstant 0.5f
    less = [a for op, a in body if op == 184]   # OpFOrdLessThan
    check(len(less) == 1 and less[0][3] in half, "main's test of a texel's alpha is not 'under 0.5'")
    stores = [a[1] for op, a in body if op == 62 and names.get(a[0]) == "any_transparent"]
    phis = {a[1]: a[2:] for op, a in body if op == 245}
    check(len(stores) == 2 and stores[0] in false_ids and stores[1] in phis and less[0][1] in phis[stores[1]],
          "any_transparent is no longer false until a texel's alpha is under 0.5")
    # the output: an array of 8-byte words, so the block written alone is 8 bytes and BC3's two halves 16
    strides = [a[2] for op, a in insts if op == 71 and a[1] == 6]   # OpDecorate ArrayStride
    check(strides == [8], "the encoder's output is not an array of 8-byte words")


def check_decoder_pinned():
    header = VK / "host_shaders" / "astc_decoder_comp_spv.h"
    digest = hashlib.sha256(header.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
    check(digest == DECODER_SHA256, "astc_decoder_comp_spv.h changed (%s): read the new decoder's errors and endpoint modes, bring "
          "gpu/vk/astc_opaque.h up to it, then put its hash into this test" % digest)
    insts, names = spirv(words_of(header))
    # what astc_opaque.h leans on, by name: the error colour is (0, 0, 0, 0), and the functions it describes are there
    for fn in ("FillError(", "FillVoidExtentLDR(", "IsError(", "DecodeBlockSize(", "DecodeMaxWeight(", "ComputeEndpoints(",
               "IsHDRColorEndpointMode(", "BlueContract(", "ClampByte(", "ReplicateByteTo16("):
        check(any(v.startswith(fn) for v in names.values()), "the ASTC decoder has no " + fn.rstrip("("))


def function(text, head):
    start = text.index(head)
    return text[start:text.index("\n}\n", start) + 3]


def check_source():
    tex = (VK / "vk_texture.cpp").read_text(encoding="utf-8")
    xfer = (VK / "vk_xfer.cpp").read_text(encoding="utf-8")
    head = (VK / "vk_texture.h").read_text(encoding="utf-8")
    # the switch and the row, and a GPU without BC1
    on = function(tex, "bool bc1_on() {")
    check('tex_bc1::wanted(getenv("RX_VK_TEX_BC1"), tex_bc1_at_start)' in on and "VK_FORMAT_BC1_RGB_UNORM_BLOCK" in on,
          "bc1_on no longer asks the switch, the row and the GPU")
    check("tex_bc1::kBc1 == VK_FORMAT_BC1_RGB_UNORM_BLOCK && tex_bc1::kBc1Srgb == VK_FORMAT_BC1_RGB_SRGB_BLOCK" in tex and
          "tex_bc1::kBc3 == VK_FORMAT_BC3_UNORM_BLOCK && tex_bc1::kBc3Srgb == VK_FORMAT_BC3_SRGB_BLOCK" in tex,
          "vk_texture.cpp no longer ties tex_bc1.h's format numbers to Vulkan's")
    # made BC1: only a texture kept as BC3 at its own size, first met by a draw that samples it, with opaque storage
    create = function(tex, "Texture* create(const Key& key")
    plain = create.index("} else if (bc_candidate && bc_ready()) {")
    made = create.index("if (!as_target && bc1_on() && astc_storage_opaque(x)) {")
    check(plain < made < create.index("const VkFormatProperties fp = format_props(x.format);"),
          "create() no longer makes a BC1 image only of a texture it keeps as BC3 at its own size")
    check("x.bc1 = true;\n                x.format = VK_FORMAT_BC1_RGB_UNORM_BLOCK;" in create, "create() does not make the image BC1")
    check("x.bc_resized = resize;" in create[:plain] and "bc1" not in create[create.index("if (bc_candidate && uneven && amd_fix) {"):plain],
          "a resized BC3 texture (its texels blitted, not the decoder's) may be made BC1")
    check("} else if (x.bc1) {" in create and
          "if (x.bc1) x.bytes = tex_bc1::bytes(x.width, x.height, x.levels, x.layers);" in create,
          "create() no longer gives a BC1 image its own flags, or counts its bytes as BC3's")
    # the encoder's switch and the views' formats
    check("const BcPush pc{(w + 3) / 4, (h + 3) / 4, w, h, dst.bc1 ? 0u : 1u, 0};" in function(tex, "void bc_encode_into("),
          "the encoder is not told to write the colour half alone for a BC1 image")
    view = function(tex, "VkImageView texture_view(")
    check("if (use != ViewUse::Sample) return VK_NULL_HANDLE;" in view and
          "f = (VkFormat)tex_bc1::view_format(tex->bc1, f == VK_FORMAT_R8G8B8A8_SRGB);" in view,
          "a kept texture's view is no longer of its image's own pair of formats, or is made for more than sampling")
    # an upload into a BC1 image: its bytes read once, checked, and those bytes staged; else BC3, whole
    up = function(tex, "bool upload_astc_gpu(")
    read = up.index("std::memcpy(own.data() + k * tail, guest_bytes(tex.gpu + at, n, scratch), n);")
    looked = up.index("opaque = opaque && astc_levels_opaque(tex, own.data() + k * tail, tail0, tail, level_align);")
    back = up.index('if (!bc1_retype(tex, "its memory holds blocks with alpha now")) {')
    again = up.index("return upload_astc_gpu(tex, total, nullptr);")
    staged = up.index("if (tex.bc1) std::memcpy(s.data, own.data(), own.size());")
    check(read < looked < back < again < up.index("const Staging s = staging(") < staged,
          "an upload into a BC1 image no longer checks the very bytes it stages before it stages them")
    check("for (size_t k = 0; k < layers.size() && !tex.bc1; k++) {" in up, "a BC1 image's upload reads the game's memory a second time")
    levels = function(tex, "bool astc_levels_opaque(")
    check("if (g.offset < tail0 || off >= tail || (off % align)) continue;" in levels and
          up.count("if (g.offset < tail0 || off >= tail || (off % align)) continue;") == 2,
          "the check no longer looks at the levels the upload decodes, by the upload's own test")
    check("Tegra::Texture::UnswizzleTexture(" in levels and "astc_opaque::all(lin.data(), n, (unsigned)tex.block_w, (unsigned)tex.block_h)" in levels,
          "the check no longer looks at a level's blocks, each once")
    # made BC3 again: a new image, the old one and its views retired, the views' cache told, the bytes counted
    back_fn = function(tex, "bool bc1_retype(Texture& x, const char* why) {")
    for needle in ("ci.format = VK_FORMAT_BC3_UNORM_BLOCK;", "destroy_gen++;", "dead.push_back(std::move(d));", "x.views.clear();",
                   "total_bytes -= std::min(total_bytes, x.bytes + x.pad);", "total_bytes += x.bytes + x.pad;",
                   "x.bc1 = false, x.bc1_no = true;", "InitRedirect redirect;"):
        check(needle in back_fn, "bc1_retype no longer has: " + needle)
    # copies on the GPU: never between a BC1 and a BC3 image, never into a BC1 image
    t2t = function(xfer, "void copy_t2t(")
    into = t2t.index('if (d.tex->bc1 && !texture_bc1_to_bc3(d.tex, "a copy on the GPU into it")) return;')
    out = t2t.index('if (s.tex->bc1 && d.tex->bc && !texture_bc1_to_bc3(s.tex, "a copy on the GPU out of it into a BC3 texture")) return;')
    check(into < out < t2t.index("end_pass(cmd);") < t2t.index("vkCmdCopyImage("),
          "copy_t2t no longer makes a BC1 texture BC3 before a copy writes it or copies its blocks into a BC3 image")
    check('if (dst->bc1 && !texture_bc1_to_bc3(dst, "a copy on the GPU into it")) return false;' in function(tex, "bool texture_copy_into_bc("),
          "texture_copy_into_bc encodes a copy into a BC1 image")
    check("bool texture_bc1_to_bc3(Texture* tex, const char* why);" in head and "bool bc1 = false;" in head and "bool bc1_no = false;" in head,
          "vk_texture.h lost the BC1 fields or texture_bc1_to_bc3")
    # nothing else writes a kept texture's image: its views are for sampling only (above), a buffer's ASTC blocks go into
    # the game's memory and are uploaded (checked again), and the texture-to-buffer copy of a decoded ASTC texture is left out
    # (the CPU's copy is b2t_on_cpu's since the parallel replay's dry pass makes it too: ASTC never falls through to the GPU's)
    b2t = function(xfer, "void copy_b2t(VkCommandBuffer cmd")
    on_cpu = function(xfer, "bool b2t_on_cpu(const NvnState& st, const nvn_p_copy_bt& p, bool write) {")
    check("if (!p.tex || (b2t_gpu_on() && !astc)) return false;" in on_cpu and "return astc;" in on_cpu and
          b2t.index("if (to_image && b2t_on_cpu(st, p, !cpu_done)) return;") < b2t.index("if (!resolve(p.tex") and
          "if (s.tex->astc && !to_image) {" in b2t, "copy_b2t may copy a buffer into a kept ASTC texture's image")


def check_menu_and_docs():
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")
    header = (VK / "vk_menu.h").read_text(encoding="utf-8")
    check(re.search(r"\bint tex_bc1 = 1;", header), "Opaque textures as BC1 is not on by default")
    check("if (version < 5) {" in function(menu, "void menu_init() {") and "opts.tex_bc1 = 1" in function(menu, "void menu_init() {"),
          "an options file from before options_version 5 does not take Opaque textures as BC1 on")
    check("int tex_bc1_at_start = 0;" in menu and "tex_bc1_at_start = opts.tex_bc1;" in function(menu, "void menu_init() {"),
          "menu_init does not keep the Opaque textures as BC1 row the game started with")
    rows = menu[menu.index("Row rows[] = {"):]
    rows = rows[:rows.index("\n};")]
    row = re.search(r'\{TAB_DEBUG, "Opaque textures as BC1", &opts\.tex_bc1, 0, 1, 1, tx_onoff,.*?hn_tex_bc1, true, true\},', rows, re.S)
    check(row and "Applies after a restart" in row.group(0), "the Opaque textures as BC1 row is not an Off/On row of the Debug tab that takes a restart")
    check(rows.index('{TAB_DEBUG, "Developer", nullptr') < rows.index('"Start next frame early", &opts.tail_early') <
          rows.index('"Opaque textures as BC1"') < len(rows),   # ((Triforce, 11-19) v0.5.2 has no Restart row: the table's end)
          "the Opaque textures as BC1 row is not among the Debug tab's Developer rows (after Start next frame early)")
    hint = function(menu, "const char* hn_tex_bc1(int, int variant) {")
    check('getenv("RX_VK_TEX_BC1")' in hint, "the row's line no longer says when the switch decides")
    readme = (ROOT / "readme.md").read_text(encoding="utf-8")
    check("| Opaque textures as BC1 | Off, On" in readme, "readme.md's settings table has no Opaque textures as BC1 row")
    guide = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    check("| `opaque_textures_as_bc1` | 0 Off, 1 On |" in guide, "docs/development.md's options table has no opaque_textures_as_bc1")
    check("| `RX_VK_TEX_BC1=0/1` |" in guide, "docs/development.md's switches table has no RX_VK_TEX_BC1")


def run_cxx(cxx):
    with tempfile.TemporaryDirectory(prefix="tex-bc1-") as tmp:
        exe = Path(tmp) / "test_tex_bc1.exe"
        base = [cxx, "-std=c++20", "-O1", "-I", str(VK), str(ROOT / "tests/test_tex_bc1.cpp")]
        # with the repository's CPU ASTC decoder where it compiles here (its own includes; -w: its warnings are not this test's;
        # NDEBUG as the build has it: its asserts stop at the first illegal block, which a release decodes as an error)
        suyu = ROOT / "gpu" / "third_party" / "suyu"
        decoder = subprocess.run([cxx, "-std=c++20", "-O1", "-w", "-DNDEBUG", "-include", "algorithm", "-include", "cstring", "-include", "vector",
                                  "-I", str(VK / "compat"), "-I", str(ROOT / "gpu" / "compat"), "-I", str(suyu), "-c",
                                  str(suyu / "video_core/textures/astc.cpp"), "-o", str(Path(tmp) / "astc.o")],
                                 capture_output=True, text=True)
        with_decoder = decoder.returncode == 0 and subprocess.run(
            base + ["-Wall", "-Wextra", "-DTEX_BC1_CPU_DECODER", str(Path(tmp) / "astc.o"), "-o", str(exe)],
            capture_output=True, text=True).returncode == 0
        if not with_decoder:
            subprocess.run(base + ["-Wall", "-Wextra", "-Werror", "-o", str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        check(r.returncode == 0, "tests/test_tex_bc1.cpp:\n" + (r.stdout + r.stderr).strip())
        return with_decoder, r.stdout.strip().splitlines()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_encoder()
    check_decoder_pinned()
    check_source()
    check_menu_and_docs()
    with_decoder, lines = run_cxx(args.cxx)
    for line in lines[:-1]:
        print(line)
    print("PASS: opaque ASTC textures as BC1: the same texels, the encoder's one colour function, the opaque blocks%s, the "
          "renderer's use of them and the row" % (" (against the CPU decoder too)" if with_decoder else
                                                  " (the CPU decoder did not compile here: authored blocks only)"))


if __name__ == "__main__":
    main()
