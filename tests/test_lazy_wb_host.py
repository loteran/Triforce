"""The lazy write-back's rule for the host (host/rx_backend.c hos_guest_io_begin, gpu/vk/vk_memory.cpp "lazy write-back").

With the Debug tab's Lazy write-back row on, pages of guest memory may have no access while the GPU's newest bytes for
them are in VRAM. The CPU's own touch faults and the renderer pays it first. The kernel does not fault: ReadFile into such
a page fails, WriteFile from one fails or writes old bytes, and a commit, discard or decommit changes the pages' protection
behind the renderer's back. So every host call that hands guest memory to the kernel, or changes guest pages' state, must
be wrapped: hos_guest_io_begin(range) before it and hos_guest_io_end after it, in the same function.

This reads host/*.c (not host/posix/, which is those calls' Linux implementation) for every call of a kernel function that
takes a buffer or changes pages (ReadFile, WriteFile, fread, fwrite, recv, send, VirtualAlloc, VirtualFree, VirtualProtect,
DiscardVirtualMemory and the like). Each must lie between a hos_guest_io_begin and a hos_guest_io_end of its function, or be
in ALLOWED below, each entry a reviewed reason why its buffer is never guest memory. A new kernel call fails here until
someone has decided which it is. An allowed call whose arguments name guest memory (GP, A64_MEM, HZ_PTR,
hos_guest_range) fails too, and so does an entry that no longer matches a call. The scanner is first run on made-up
sources that must fail, so that it cannot pass by finding nothing.

And the two ends meet: rx_backend.c defines both calls and looks the renderer's two exports up by name, and
gpu/vk/vk_memory.cpp exports them.

    python -I -B tests/test_lazy_wb_host.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

KERNEL = ("ReadFile", "ReadFileEx", "WriteFile", "WriteFileEx", "fread", "fwrite", "_read", "_write", "pread", "pwrite",
          "recv", "recvfrom", "send", "sendto", "WSARecv", "WSASend", "DeviceIoControl", "VirtualAlloc", "VirtualAllocEx",
          "VirtualFree", "VirtualProtect", "DiscardVirtualMemory", "OfferVirtualMemory", "ReclaimVirtualMemory",
          "ReadProcessMemory", "WriteProcessMemory", "discard")
CALL = re.compile(r"(?<![\w.>])(" + "|".join(KERNEL) + r")\s*\(")
GUEST = re.compile(r"\b(GP|A64_MEM|HZ_PTR|hos_guest_range|G8P|G16P|G32P|G64P)\b")

# (file, function, call): why its buffer or pages are never guest memory. Reviewed one by one (2026-11-15)
ALLOWED = {
    ("bug_capture.c", "read_part", "fread"): "into a buffer it mallocs",
    ("nn_fs.c", "hos_fs_rom_file", "ReadFile"): "a RomFS file for the menu sounds (host/menu_sound.c), into a buffer it mallocs",
    ("bug_capture.c", "write_file", "fwrite"): "the report's own files: the options file and a log read into host memory",
    ("discord_rpc.c", "ipc_write", "WriteFile"): "Discord's pipe: a JSON message the host built",
    ("discord_rpc.c", "ipc_read", "ReadFile"): "Discord's pipe: into the host's buffer",
    ("discord_rpc.c", "ipc_write", "send"): "Discord's socket (Linux): a JSON message the host built",
    ("discord_rpc.c", "ipc_read", "recv"): "Discord's socket (Linux): into the host's buffer",
    ("hos_main.c", "reserve_large", "VirtualAlloc"): "the guest window's large pages, reserved at the start before any "
                                                     "renderer runs",
    ("hos_main.c", "reserve_guest", "VirtualAlloc"): "the guest window's reservation at the start, before any renderer runs",
    ("hos_main.c", "reserve_guest", "VirtualFree"): "the same reservation given up again at the start (a large-page try)",
    ("hos_main.c", "copy_tree", "fread"): "a file copy through a stack buffer",
    ("hos_main.c", "copy_tree", "fwrite"): "a file copy through a stack buffer",
    ("mods.c", "zs_content_size", "fread"): "(Triforce) a mod file's 18-byte zstd header into a stack buffer",
    ("hos_mem.c", "census_alloc", "VirtualAlloc"): "the guest memory census's own bitmaps, host memory the system hands out",
    ("hos_mem.c", "census_free", "VirtualFree"): "the same bitmaps, a chunk made twice by two threads given up",
    ("hos_thread.c", "log_write", "fwrite"): "the log's lines, host memory",
    ("hos_thread.c", "log_drain_raw", "WriteFile"): "a log line, a host buffer",
    ("hos_update.c", "pipe_tag", "fread"): "a stack buffer",
    ("mod_cheats.c", "hos_mod_cheats_add", "fread"): "into a buffer it mallocs",
    ("mods.c", "read_whole", "fread"): "into a buffer it reallocs",
    ("nn_audio.c", "output_open", "fwrite"): "a WAV header on the stack (HOS_AUDIO_DUMP)",
    ("nn_audio.c", "dump_finish", "fwrite"): "the WAV header, on the stack",
    ("nn_audio.c", "output_frame", "fwrite"): "the host's mixed samples",
    ("nn_audio.c", "HLE(_ZN2nn5audio16AppendWaveBufferEPNS0_9VoiceTypeEPKNS0_10WaveBufferE)", "fwrite"):
        "the voice's coefficients, host memory (the wave buffer after them is wrapped)",
    ("nn_nfp.c", "load_dump", "fread"): "a stack buffer",
    ("nvn_native.c", "wts_handler", "VirtualProtect"): "a diagnostic switch's own protection of guest pages "
                                                      "(RX_NVN_WATCH_TEX, RX_NVN_WATCH_TEXELS_SRC): not for use with the lazy write-back",
    ("nvn_native.c", "wts_rearm", "VirtualProtect"): "the same diagnostic",
    ("nvn_native.c", "hos_watch_guest_writes", "VirtualProtect"): "the same diagnostic",
    ("nvn_native.c", "watch_texels_source", "VirtualProtect"): "the same diagnostic",
    ("pgo_rt.c", "pgo_write", "fwrite"): "the profile's counters, host memory",
    ("prompts.c", "zstd_block", "fwrite"): "a font the host read into a buffer it mallocs",
    ("prompts.c", "write_zstd_raw", "fwrite"): "a header on the stack",
    ("prompts.c", "read_all", "fread"): "into a buffer it mallocs",
    ("prompts.c", "write_patched", "fwrite"): "a header on the stack and chunks of its own buffer",
    ("prompts.c", "write_patched", "fread"): "into a buffer it mallocs",
    ("save_share.c", "read_file", "fread"): "into a buffer it mallocs",
    ("save_share.c", "write_whole", "fwrite"): "a zip the host built",
}
# (the same, for calls on guest memory that the lazy write-back cannot have taken: no wrap, and why)
ALLOWED_GUEST = {
    ("mkw_glue.c", "commit_on_touch", "VirtualAlloc"): "the host's own fault handler committing a page that is only reserved "
                                                       "(it asks VirtualQuery first): every page the lazy write-back protects "
                                                       "is committed, imported whole, and its handler comes first",
    ("hos_mem.c", "census_release", "VirtualFree"): "the census's give-back: its caller census_offer wraps the renderer's call "
                                                    "that runs it in hos_guest_io_begin / hos_guest_io_end; inside, the renderer "
                                                    "holds its import locks, under which the lazy write-back's begin is not taken",
}


def strip(text):
    """Comments and string and character literals blanked out (their newlines kept), so that braces and names in them
    count for nothing"""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(n, j + 1)
            out.append(c + re.sub(r"[^\n]", " ", text[i + 1:j - 1]) + (c if j <= n else ""))
            i = j
        elif c == "#" and (i == 0 or text[i - 1] == "\n"):   # (a preprocessor line: its braces are not the code's)
            j = i
            while True:
                j = text.find("\n", j)
                if j < 0 or text[j - 1] != "\\":
                    break
                j += 1
            j = n if j < 0 else j
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def functions(code):
    """(name, start, end) of each top-level brace block, its name the first name( of its head: a function's name, or
    HLE(<symbol>) for the host's HLE functions"""
    blocks, depth, head_from, start = [], 0, 0, 0
    for i, c in enumerate(code):
        if c == "{":
            if depth == 0:
                start = i
                head = code[head_from:i]
                m = re.search(r"\b(HLE)\s*\(\s*(\w+)\s*\)", head)
                if m:
                    name = "HLE(%s)" % m.group(2)
                else:
                    m = re.search(r"(\w+)\s*\(", head)
                    name = m.group(1) if m else head.strip().split("\n")[-1].strip()
                blocks.append([name, start, None])
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                blocks[-1][2] = i
                head_from = i + 1
        elif c == ";" and depth == 0:
            head_from = i + 1
    return [(b[0], b[1], b[2] if b[2] is not None else len(code)) for b in blocks]


def args_of(code, at):
    """The argument text of the call whose "(" is at"""
    depth, i = 0, at
    while i < len(code):
        depth += (code[i] == "(") - (code[i] == ")")
        if depth == 0:
            return code[at + 1:i]
        i += 1
    return code[at + 1:]


def scan(name, text):
    """[(function, call, line, args, wrapped)] for every kernel call in one source"""
    code = strip(text)
    found = []
    for fn, lo, hi in functions(code):
        body = code[lo:hi]
        for m in CALL.finditer(body):
            at = lo + m.start()
            before, after = code[lo:at], code[at:hi]
            b, e = before.rfind("hos_guest_io_begin("), before.rfind("hos_guest_io_end(")
            wrapped = b >= 0 and b > e and "hos_guest_io_end(" in after
            found.append((fn, m.group(1), code.count("\n", 0, at) + 1, args_of(code, lo + m.end() - 1), wrapped))
    return found


def judge(files):
    """The problems with these sources ({file name: text}) under the rule, and the ALLOWED entries that matched"""
    problems, used = [], set()
    for name, text in sorted(files.items()):
        for fn, call, line, args, wrapped in scan(name, text):
            key = (name, fn, call)
            if wrapped:
                continue
            if key in ALLOWED_GUEST:
                used.add(key)
                continue
            if key in ALLOWED:
                used.add(key)
                if GUEST.search(args):
                    problems.append("%s:%d: %s(%s) in %s is allowed as host memory, yet it names guest memory: wrap it in "
                                    "hos_guest_io_begin / hos_guest_io_end" % (name, line, call, args.strip()[:60], fn))
                continue
            problems.append("%s:%d: %s(%s) in %s goes to the kernel without hos_guest_io_begin / hos_guest_io_end around "
                            "it. If its buffer or pages can be guest memory, wrap it (host/rx_backend.c); if they never can, "
                            "add (\"%s\", \"%s\", \"%s\") with the reason to ALLOWED in tests/test_lazy_wb_host.py"
                            % (name, line, call, args.strip()[:60], fn, name, fn, call))
    return problems, used


def self_test():
    """The scanner has teeth: these must fail, those must pass"""
    bad = {
        "a.c": "static void r(int h, uint64_t b, unsigned n) {\n    DWORD got;\n    ReadFile(h, GP(b), n, &got, NULL);\n}\n",
        "b.c": "HLE(_Zfoo) {\n    const int io = hos_guest_io_begin(GP(ARG(0)), 8);\n    hos_guest_io_end(io);\n"
               "    WriteFile(h, GP(ARG(0)), 8, &put, NULL);\n}\n",
        "c.c": "static void w(void) {\n    const int io = hos_guest_io_begin(p, n);\n    fwrite(GP(x), 1, n, f);\n}\n",
        "bug_capture.c": "static int write_file(const char *path, const void *data, size_t n) {\n"
                         "    return fwrite(GP(data), 1, n, f) == n;\n}\n",
    }
    for name, text in bad.items():
        problems, _ = judge({name: text})
        if not problems:
            raise SystemExit("FAIL: the scanner passes an unwrapped kernel call on guest memory in:\n" + text)
    good = {
        "d.c": "static void r(int h, uint64_t b, unsigned n) {\n    /* ReadFile(h, GP(b)) in a comment */\n"
               "    const char *s = \"fwrite(GP(x))\";\n    const int io = hos_guest_io_begin(GP(b), n);\n"
               "    if (n) { ReadFile(h, GP(b), n, &got, NULL); }\n    hos_guest_io_end(io);\n}\n",
        "e.c": "static int f(void) { return x.read(1) + y->write(2) + my_fread(3); }\n",
    }
    for name, text in good.items():
        problems, _ = judge({name: text})
        if problems:
            raise SystemExit("FAIL: the scanner fails a wrapped call or no call at all:\n" + "\n".join(problems))


def main():
    self_test()
    files = {p.name: p.read_text(encoding="utf-8", errors="replace") for p in sorted((ROOT / "host").glob("*.c"))}
    problems, used = judge(files)
    for key in sorted((set(ALLOWED) | set(ALLOWED_GUEST)) - used):
        problems.append("ALLOWED has %r, which matches no unwrapped kernel call any more: take it out" % (key,))
    backend = (ROOT / "host/rx_backend.c").read_text(encoding="utf-8")
    for need in ("int hos_guest_io_begin(const void *p, uint64_t n)", "void hos_guest_io_end(int held)",
                 "GetProcAddress(vk_module, \"mkwvk_lazy_io_begin\")", "GetProcAddress(vk_module, \"mkwvk_lazy_io_end\")"):
        if need not in strip(backend) and need not in backend:
            problems.append("host/rx_backend.c no longer has " + need)
    memory = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    for need in ("int mkwvk_lazy_io_begin(const void* p, uint64_t n)", "void mkwvk_lazy_io_end(int held)"):
        if need not in memory:
            problems.append("gpu/vk/vk_memory.cpp no longer exports " + need)
    if problems:
        print("\n".join("FAIL: " + p for p in problems))
        sys.exit(1)
    n = sum(len(scan(k, v)) for k, v in files.items())
    print("PASS: %d kernel calls in host/*.c, each wrapped or allowed with a reason (%d allowed)" % (n, len(used)))


if __name__ == "__main__":
    main()
