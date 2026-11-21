"""The game's texture pool has 33,068 descriptors: every id of it registers and answers its own texture.

NX2navi, the same game on another renderer, kept its pool textures in a table of 32,768 and its translated shaders clamped a
pool handle to 32,767: the textures the game numbers from 32,768 up were drawn with whatever slot 32,767 held (see-through
ground until the area was loaded again). This renderer has no such table: a draw's handles are resolved on the CPU, the
texture id being the handle's low 20 bits, through registrations that grow to the id (gpu/vk/vk_bank.cpp), and no shader
indexes a pool. This check keeps it so.

It runs vk_bank.cpp's registrations, cut from the source, with the game's whole pool (ids 256 to 33,067) and with the ends
of the handle's range (20 bits of texture id, 12 of sampler id), and reads the places an id passes on its way: the limits
the game is told (host/nvn_native.c devinfo: a pool of up to 1,048,576 textures and 4,096 samplers), the pool's
registration proc, the queued registration's range check, and the draw's split of a handle.

python -I -B tests/test_texture_pool_ids.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

CASES = ["the-games-pool", "the-handles-range", "registered-again", "samplers"]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(where, text, needles):
    for needle in needles:
        if needle not in text:
            raise SystemExit(where + " no longer has: " + needle)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    read = lambda path: (a.source / path).read_text(encoding="utf-8")
    bank, native, replay, draw, nvn_api = (read(path) for path in ["gpu/vk/vk_bank.cpp", "host/nvn_native.c", "gpu/vk/vk_replay.cpp",
                                                                 "gpu/vk/vk_draw.cpp", "gpu/render/nvn_api.h"])
    # (what the game is told of the pools' sizes: NVN's own limits, which a handle's 20 and 12 bits hold)
    need("gpu/render/nvn_api.h", nvn_api, ["NVN_DEVICE_INFO_MAX_TEXTURE_POOL_SIZE = 0x24,", "NVN_DEVICE_INFO_MAX_SAMPLER_POOL_SIZE = 0x25,"])
    devinfo = native[native.index("static const int devinfo[] = {"):]
    devinfo = devinfo[:devinfo.index("};")]
    told = {int(k, 16): int(v) for k, v in re.findall(r"\[0x([0-9A-Fa-f]+)\] = (\d+)", devinfo)}
    if told.get(0x24) != 1 << 20 or told.get(0x25) != 1 << 12:
        raise SystemExit("host/nvn_native.c: the game is told other pool sizes than a handle holds: textures %s, samplers %s"
                         % (told.get(0x24), told.get(0x25)))
    need("host/nvn_native.c", native,
         # (the id goes on as the game gave it, whatever the pool's size; images share the texture pool's ids)
         ["NV(nvnTexturePoolRegisterTexture) { rx_nvn_register_texture(I(1), (const nvn_tex *)P(2), (const nvn_view *)P(3)); }",
          "NV(nvnTexturePoolRegisterImage) { rx_nvn_register_texture(I(1), (const nvn_tex *)P(2), (const nvn_view *)P(3)); }",
          "NV(nvnSamplerPoolRegisterSampler) { rx_nvn_register_sampler(I(1), &((const nvn_samp *)P(2))->p); }",
          "RET(t ? (uint64_t)t | (uint64_t)s << 20 : 0);"])
    need("vk_replay.cpp", replay, ["void set_texture_pool_size(int) {}"])   # (no table is sized by the pool)
    need("vk_replay.cpp: register_texture_queued()", block(replay, "void register_texture_queued(int id"),
         ["if (id < 0 || id >= (1 << 20) || !t) return;", "j.id = id, j.tex = *t;"])
    need("vk_replay.cpp: register_sampler_queued()", block(replay, "void register_sampler_queued(int id"),
         ["if (id < 0 || id >= (1 << 12) || !p) return;"])
    need("vk_replay.cpp", replay, ["    int id = 0;                 // (a registration's slot, its view and sampler)"])
    need("vk_bank.cpp: register_texture()", block(bank, "void register_texture(int id"), ["if (id < 0 || id >= (1 << 20) || !t) return;"])
    need("vk_bank.cpp", bank, ["std::vector<TexReg> tex_regs;", "constexpr int kSlotVersions = 1 << 20;", "constexpr int kSampVersions = 1 << 12;"])
    # (a draw's handle: the texture id its low 20 bits, the sampler id the 12 above; each looked up, none used as an index of
    # a table of the draw's own)
    need("vk_draw.cpp: texture_view_of()", block(draw, "VkImageView texture_view_of(uint32_t handle"),
         ["const uint32_t version = texture_slot_version((int)(handle & 0xFFFFF));",
          "const bool known = texture_of((int)(handle & 0xFFFFF), &t, &v, &has_view);"])
    # ((Triforce) the sampled textures' sampler through kept_texture, the per-binding reuse: the whole handle its input)
    need("vk_draw.cpp: kept_texture()", block(draw, "VkSampler kept_texture("),
         ["const VkSampler smp = sampler_for((int)(handle >> 20), compare, scene);", "in = handle | (uint64_t)vt << 32"])
    need("vk_draw.cpp", draw, ["kept_texture(kp, jb + k, handle, vt, d.is_depth,", "dw().imgs.push_back({smp, view, VK_IMAGE_LAYOUT_GENERAL});",
                               "if (!on || id < 0 || id >= 4096) return sampler_make(id, compare, scene);"])
    for where, text in [("vk_draw.cpp", draw), ("vk_bank.cpp", bank), ("vk_replay.cpp", replay)]:
        for mask in re.findall(r"handle & (0x[0-9A-Fa-f]+)", text):
            if int(mask, 16) != 0xFFFFF:
                raise SystemExit("%s takes a texture id of other than 20 bits from a handle: handle & %s" % (where, mask))

    tables = bank[bank.index("struct TexReg {"):]
    tables = tables[:tables.index("std::unique_ptr<std::atomic<uint32_t>[]> samp_versions{")]
    tables += "std::unique_ptr<std::atomic<uint32_t>[]> samp_versions{new std::atomic<uint32_t>[kSampVersions]()};\n"
    functions = "\n".join(block(bank, marker) for marker in [
        "void register_texture_copy(int id, const nvn_tex& tex, const nvn_view* v) {", "uint32_t texture_slot_version(int id) {",
        "uint64_t registrations_generation() {", "void register_sampler_copy(int id, const nvn_samp_par& par) {",
        "uint32_t sampler_slot_version(int id) {", "void texture_released(uint64_t serial) {",
        "bool texture_of(int id, nvn_tex* t, nvn_view* v, bool* has_view) {", "bool sampler_of(int id, nvn_samp_par* p) {"])
    fixture = (ROOT / "tests/texture_pool_ids_test.cpp").read_text(encoding="utf-8")
    for marker, text in [("/* TABLES */", tables), ("/* FUNCTIONS */", functions)]:
        assert fixture.count(marker) == 1, marker
        fixture = fixture.replace(marker, text)
    with tempfile.TemporaryDirectory(prefix="texture-pool-ids-") as temp:
        cpp, exe = Path(temp) / "fixture.cpp", Path(temp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        for case in CASES:
            subprocess.run([str(exe), case], check=True, timeout=30)
    print(f"{len(CASES)} texture pool id cases passed")


if __name__ == "__main__":
    main()
