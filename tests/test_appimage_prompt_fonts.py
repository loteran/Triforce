"""Game-free: the Linux AppImage carries the controller fonts the button prompts are drawn from (third_party/kenney_input,
nx2/prompt_assets.py KENNEY). Without them make_game.py says the prompts "were not made" and the game keeps the Switch's icons,
even with fontTools installed. And the font the keyboard and mouse face's labels are set in (third_party/fonts,
nx2/prompt_keys.py LETTERS).

python -I tests/test_appimage_prompt_fonts.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    script = (ROOT / "scripts/linux/appimage.sh").read_text(encoding="utf-8")
    assets = (ROOT / "nx2/prompt_assets.py").read_text(encoding="utf-8")
    m = re.search(r'KENNEY = os\.path\.join\(os\.path\.dirname\(HERE\), "third_party", "([^"]+)"\)', assets)
    if not m:
        raise SystemExit("FAIL: nx2/prompt_assets.py has no KENNEY under third_party")
    folder = "third_party/" + m.group(1)
    if not (ROOT / folder).is_dir():
        raise SystemExit("FAIL: %s does not exist" % folder)
    if not re.search(r'^cp -a %s "\$T/third_party/"$' % re.escape(folder), script, re.M):
        raise SystemExit("FAIL: scripts/linux/appimage.sh does not copy %s" % folder)
    # ((Triforce) the keyboard and mouse face's labels: a font of third_party/fonts (nx2/prompt_keys.py LETTERS). Without it
    # that face alone is not made, and the game shows its own icons for it)
    keys = (ROOT / "nx2/prompt_keys.py").read_text(encoding="utf-8")
    m = re.search(r'LETTERS = os\.path\.join\(os\.path\.dirname\(HERE\), "third_party", "([^"]+)", "([^"]+)", "([^"]+)"\)', keys)
    if not m:
        raise SystemExit("FAIL: nx2/prompt_keys.py has no LETTERS under third_party")
    labels = "third_party/" + m.group(1)
    if not (ROOT / labels / m.group(2) / m.group(3)).is_file():
        raise SystemExit("FAIL: %s/%s/%s does not exist" % (labels, m.group(2), m.group(3)))
    if not re.search(r'^cp -a [^"\n]*\b%s [^\n]*"\$T/third_party/"$' % re.escape(labels), script, re.M) and \
            not re.search(r'^cp -a [^"\n]*\b%s "\$T/third_party/"$' % re.escape(labels), script, re.M):
        raise SystemExit("FAIL: scripts/linux/appimage.sh does not copy %s" % labels)
    print("ok: the AppImage copies %s and %s" % (folder, labels))


if __name__ == "__main__":
    sys.exit(main())
