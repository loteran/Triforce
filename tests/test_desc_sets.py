"""A program over the push limit: its set written through an update template, and a set written alike kept and bound again.

vk_draw.cpp's set path (bind_descriptors, set_kept, set_written, set_template, set_write_template) is cut out of the source and run
against a fake driver that writes each set's descriptors into a store of its own, from the write structs or from a template's entries
as the spec has the driver read them (offset + element * stride). Every bind checks that the set bound holds exactly the descriptors
the draw built, whichever path wrote it. The cases (tests/desc_sets_test.cpp):
  reuse            the same descriptors again: the set written before, bound again; one allocation, one write
  changed          a buffer, a view or a texel view otherwise: a new set
  destroy          a texture destroyed or made again since (texture_destroy_generation): a new set (issue 1126)
  stamp            a pool moved, a mirror made or the next submission (gpu_bind_stamp): a new set
  recording        another recording (its pool was reset): a new set
  cmd, bindpoint   another command buffer; each bind point keeps its own
  layout           another layout with the same words: a set of its own, the first layout's still kept
  keep-off         RX_VK_DESC_SET_KEEP=0: a new set each time, as before
  template         written through the program's one template, the same descriptors as the write structs give
  template-off     RX_VK_DESC_TEMPLATE=0: the write structs
  template-misfit  writes that are not those the template was made from: the write structs
  template-fail    no template could be made: the write structs, and not tried again at every set
  template-entries the entries' offsets and strides; writes that are not one a binding from its first element: no template
  push             a program within the push limit: pushed in full (through its push template), then nothing, then the changed write
                   by its write struct, and counted; what the bind point holds is what the draw built
  push-template    (Triforce) one push template a program and bind point, for its pipeline layout's set 0; another command buffer
                   pushes whole again through it, another program or a layout made again gets its own; the recorder is told the
                   bytes the driver reads
  push-template-off, push-template-none, push-template-misfit
                   RX_VK_DESC_TEMPLATE=0, no vkCmdPushDescriptorSetWithTemplateKHR, writes it was not made from or none made (and not
                   tried again): the write structs, as before
Each case fails without the rule it names (remove the check from set_kept, and its case stops); the push template's rules are
taken out in turn here (MUTATIONS) and a case must then fail.
python -I -B tests/test_desc_sets.py --headers <Vulkan include folder> [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]
CASES = ["reuse", "changed", "destroy", "stamp", "recording", "cmd", "bindpoint", "layout", "keep-off", "template", "template-off",
         "template-misfit", "template-fail", "template-entries", "push", "push-template", "push-template-off", "push-template-none",
         "push-template-misfit"]
# ((Triforce) the push template's rules, each taken out of vk_draw.cpp: the case named must then fail)
MUTATIONS = [
    ("the descriptors packed for the push", "    template_pack(dw().writes.data(), dw().writes.size(), t->slots, data);\n    rec::push_template_bytes(",
     "    template_pack(dw().writes.data(), dw().writes.size(), t->slots, data);\n    std::memset(data.data(), 0, data.size());\n"
     "    rec::push_template_bytes(", "push"),
    ("the recorder told the bytes", "rec::push_template_bytes(data.size());", "rec::push_template_bytes(0);", "push"),
    ("a template for each pipeline layout", "if (!t || t->pipe_layout != p.layout) {", "if (!t) {", "push-template"),
    ("writes that fit the push template", "    if (!t || !t->tpl || !template_fits(t->sig, dw().writes.data(), dw().writes.size())) return false;\n"
     "    std::vector<uint8_t>& data = tpl_data();\n    template_pack(dw().writes.data(), dw().writes.size(), t->slots, data);\n"
     "    rec::push_template_bytes(", "    if (!t || !t->tpl) return false;\n    std::vector<uint8_t>& data = tpl_data();\n"
     "    template_pack(dw().writes.data(), dw().writes.size(), t->slots, data);\n    rec::push_template_bytes(", "push-template-misfit"),
    ("the switch's 0 for pushes", "if (!template_on() || !vkCreateDescriptorUpdateTemplate || !vkCmdPushDescriptorSetWithTemplateKHR",
     "if (!vkCreateDescriptorUpdateTemplate || !vkCmdPushDescriptorSetWithTemplateKHR", "push-template-off"),
]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def fixture_of(source):
    counts_at = source.index("std::atomic<int64_t> st_dk_builds{0}")
    counts = source[counts_at:source.index(block(source, "inline int64_t descs_of(")) + len(block(source, "inline int64_t descs_of("))]
    words = block(source, "struct Pushed {") + ";\n" + "\n".join(
        block(source, m) for m in ["void write_words(", "void words_append(", "int words_same_store("])
    sets = source[source.index("// ---- (Triforce) The sets of programs over the push limit"):source.index("bool bind_descriptors(VkCommandBuffer")]
    bind = block(source, "bool bind_descriptors(")
    # (the rules this test stands on, where the cut text must keep them)
    kept = block(sets, "VkDescriptorSet set_kept(")
    for rule in ["k.cmd != cmd", "k.layout != layout", "k.rec != desc_rec", "k.destroy != texture_destroy_generation()",
                 "k.stamp != gpu_bind_stamp()", "k.words != w"]:
        if rule not in kept:
            raise SystemExit("set_kept no longer checks %s" % rule)
    tail = bind[bind.index("ps.cmd = VK_NULL_HANDLE;   // (a set bound"):]
    if tail.index("set_kept(") > tail.index("VkDescriptorSetAllocateInfo ai"):
        raise SystemExit("a kept set is looked for after a new one is allocated")
    if tail.index("set_write_template(") > tail.index("vkUpdateDescriptorSets(") or "set_written(" not in tail:
        raise SystemExit("the set path does not try the template first, or does not keep the set it wrote")
    # ((Triforce) a whole push tries the push template first, the write structs when it gives none; the changed bindings by theirs)
    full = bind[:bind.index("static thread_local std::vector<VkWriteDescriptorSet> changed")]
    if not re.search(r"if \(!push_write_template\(cmd, bp, p\)\)[^\n]*\n\s*vkCmdPushDescriptorSetKHR\(cmd, bp, p\.layout, 0, "
                     r"\(uint32_t\)dw\(\)\.writes\.size\(\), dw\(\)\.writes\.data\(\)\);", full):
        raise SystemExit("a whole push does not try the push template first, with the write structs when it gives none")
    if "push_write_template(" in bind[len(full):]:
        raise SystemExit("a push of the changed bindings only goes through the push template")
    fixture = (ROOT / "tests" / "desc_sets_test.cpp").read_text(encoding="utf-8")
    for mark, text in [("/* COUNTS */", counts), ("/* WORDS */", words), ("/* SETS */", sets), ("/* BIND */", bind)]:
        fixture = fixture.replace(mark, text)
    return fixture


def build(cxx, headers, tmp, name, fixture):
    cpp, exe = Path(tmp) / (name + ".cpp"), Path(tmp) / (name + ".exe")
    cpp.write_text(fixture, encoding="utf-8")
    subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers", "-pthread",
                    "-I" + str(headers), str(cpp), "-o", str(exe)], check=True)
    return exe


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--headers", type=Path, required=True)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--source", type=Path, default=ROOT / "gpu" / "vk" / "vk_draw.cpp", help="another vk_draw.cpp (a mutation)")
    a = p.parse_args()
    source = a.source.read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="desc-sets-") as tmp:
        exe = build(a.cxx, a.headers, tmp, "fixture", fixture_of(source))
        for case in CASES:
            subprocess.run([str(exe), case], check=True, timeout=20)
        print("PASS: %d cases: kept sets and update templates give the descriptors the draws built" % len(CASES))
        for i, (what, old, new, case) in enumerate(MUTATIONS):
            if source.count(old) != 1:
                raise SystemExit("mutation %r: its text is not in vk_draw.cpp once (update the test with the code)" % what)
            exe = build(a.cxx, a.headers, tmp, "mutant%d" % i, fixture_of(source.replace(old, new)))
            r = subprocess.run([str(exe), case], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=20)
            if r.returncode == 0:
                raise SystemExit("mutation %r: case %s still passed without it" % (what, case))
            print("PASS mutation: without %s, case %s fails (%s)" % (what, case, r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.returncode))


if __name__ == "__main__":
    main()
