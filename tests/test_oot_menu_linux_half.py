"""host/oot_menu.c's two halves: what other host files call is defined in both.

The file is one half on Windows (#if defined(_WIN32)) and another elsewhere (#else). A function the Windows half added for the
rest of the host (v0.5.0's hos_oot_mods_down, nn_hid.c) and not the other half stopped every Linux build at the link (11-18).
This test lists the non-static functions each half defines and the ones host/*.c outside it call, and checks that every one
called from outside is in the non-Windows half too.
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "host" / "oot_menu.c"
DEF = re.compile(r"^(?!static\b)[A-Za-z_][\w \t\*]*?\b(hos_\w+)\s*\([^;]*\)\s*\{", re.M)


def halves(text):
    start = text.index("#if defined(_WIN32)")
    other = text.index("\n#else", start)
    end = text.index("\n#endif", other)
    return text[start:other], text[other:end]


class OotMenuHalvesTest(unittest.TestCase):
    def test_called_from_outside_defined_in_both(self):
        text = SRC.read_text(encoding="utf-8")
        win, other = halves(text)
        win_defs, other_defs = set(DEF.findall(win)), set(DEF.findall(other))
        self.assertIn("hos_oot_menu_tick", other_defs)   # (the parse found the other half's functions)
        outside = "\n".join(p.read_text(encoding="utf-8", errors="replace")
                            for p in sorted((ROOT / "host").glob("*.c")) if p.name != "oot_menu.c")
        called = {name for name in win_defs if re.search(r"\b%s\s*\(" % name, outside)}
        self.assertIn("hos_oot_mods_down", called)
        missing = sorted(called - other_defs)
        self.assertEqual(missing, [], "defined only in oot_menu.c's Windows half, but called from other host files: %s" % missing)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
