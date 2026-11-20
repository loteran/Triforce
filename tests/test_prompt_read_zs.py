"""nx2/prompt_assets.py read_zs: a game file is decompressed a piece at a time and the pieces joined once.

Given the whole file in one call, the zstandard package grows one bytes object by copying as the output comes: the game's
interface textures (69 MB packed, 244 MB unpacked) took 20 s of the button prompts' 80 that way (NX2navi's
tools/pad_prompts.py unpack found the same, and reads in pieces). What comes out must be what went in, whatever the
file's size against a piece's.

Game-free: files written here, packed with a dictionary written here.

    python -I -B tests/test_prompt_read_zs.py
"""
import os
import sys
import tempfile
import time
from pathlib import Path

RUN_ALL_MODULES = ["zstandard"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import prompt_assets  # noqa: E402


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def main():
    import zstandard
    dictionary = b"a dictionary written for this test: " + bytes(range(256)) * 8
    packer = zstandard.ZstdCompressor(level=3, dict_data=zstandard.ZstdCompressionDict(dictionary))
    piece = 1 << 16
    # (nothing, less than a piece, a piece to the byte, several pieces of what packs badly and of what packs well)
    cases = {"empty": b"", "small": b"some text " * 20, "noise": os.urandom(5 * piece + 123), "plain": bytes(range(256)) * 40000}
    with tempfile.TemporaryDirectory() as tmp:
        for name, data in cases.items():
            packed = packer.compress(data)
            Path(tmp, name + ".zs").write_bytes(packed)
            check(prompt_assets.read_zs(tmp, name + ".zs", dictionary) == data, "%s: not what was packed" % name)
            Path(tmp, name + ".more.zs").write_bytes(packed + b"bytes after the frame")
            check(prompt_assets.read_zs(tmp, name + ".more.zs", dictionary) == data, "%s: bytes after the frame came out" % name)
        check(len(Path(tmp, "noise.zs").read_bytes()) > 5 * piece, "the noise packed into less than several pieces")
        started = time.perf_counter()
        prompt_assets.read_zs(tmp, "plain.zs", dictionary)
        took = time.perf_counter() - started
    body = (ROOT / "nx2/prompt_assets.py").read_text(encoding="utf-8")
    body = body[body.index("def read_zs("):body.index("def size_table(")]
    check("decompressobj()" in body and "for at in range(0, len(raw)," in body and 'b"".join(parts)' in body,
          "read_zs no longer reads in pieces")
    print("ok: %d files read as packed (10 MB in %.2f s)" % (2 * len(cases), took))


if __name__ == "__main__":
    main()
