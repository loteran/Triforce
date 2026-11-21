"""The launcher's music loops as the game's does and is not cut off: the page (launcher/ui/app.js) and the launcher
(launcher/Core/GameFiles) agree on the loop, read from the source and run in node (no launcher is built or started, no
game file is read, and no sound is made).

The game's music plays an intro once and then loops from a sample after it to its end (Saria's Song, the Fairy
Fountain and the Temple of Time; the BWAV header gives the loop). The page played the WAV in an <audio> element with loop on, which starts the whole file again, intro and
all, after a pause, and the end does not join the start (a step in the wave, a click). Now the launcher keeps the loop
in the WAV as a smpl chunk (Bwav.ToWav; its reading and writing are tested in launcher/LogicTests: dotnet run --project
launcher/LogicTests media) and the page plays music through Web Audio, looping a decoded buffer between those points.

This checks what joins the two, and runs the page's music code (from "const TRACKS" to the end of audioTick) in node
with stand-ins for Web Audio, fetch and <audio> (tests/launcher_music_test.js):

- the page reads the loop where the launcher writes it (a smpl chunk's first loop, its last sample counted in), and a
  file without one, an older launcher's, loops whole;
- a track is read once, loops between its points at the game's rate, fades in once it is read, follows the volume and
  mute, fades out whole before it stops, and lets go of its samples; a track faded out before it is read never starts;
- the item fanfare plays once at its full level from its first note, and what plays after it is asked for when it ends;
- a context the browser holds back resumes on the first click or key; without Web Audio, or for a file it cannot
  decode, the <audio> element plays as before;
- the music each screen plays and its fades are as before, and the WAV cache is made again once for the loop (the art's
  is not).

    python -I -B tests/test_launcher_music.py [--node node]
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--node", "{node}"]

ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = ROOT / "launcher"


def read(path):
    return path.read_text(encoding="utf-8")


def need(text, needle, why):
    if needle not in text:
        raise SystemExit(why + "\n  (looked for: " + needle + ")")


def check_source(app, bwav, media):
    # (the screens' music and fades as before: the page plays the same tracks at the same places)
    need(app, "const MUSIC = { welcome: null, reveal: 'saria', work: 'temple', done: 'itemget', home: 'fairy', saves: 'fairy', "
              "mods: 'fairy', settings: 'fairy', bench: 'fairy', brun: null, playing: null };",
         "launcher/ui/app.js: the music a screen plays changed")
    need(app, "if (name === 'done') music('itemget', false, .3);", "launcher/ui/app.js: the fanfare on the done screen changed")
    need(app, "music(m, true, name === 'playing' || name === 'brun' ? 2.2 : 1.4);", "launcher/ui/app.js: the fades between screens changed")
    need(app, "function music(name, loop = true, fade = 1.4) {", "launcher/ui/app.js: music() takes other arguments")
    # (Web Audio first, looping between the loop points; the <audio> element only as the fallback)
    for needle in ("src.loop = true; src.loopStart = t.start; src.loopEnd = t.end;", "actx.decodeAudioData(ab, ok, no)",
                   "new AC({ sampleRate: 48000 })", "if (audioCtx()) track(name).then(t => { if (!m.dead) startBuffer(m, t); }, () => { if (!m.dead) startElement(m); });",
                   "else startElement(m);", "addEventListener('pointerdown', unlock, true); addEventListener('keydown', unlock, true);"):
        need(app, needle, "launcher/ui/app.js: the music no longer plays through Web Audio with its loop, or has no fallback")
    sound = app[app.index("const TRACKS = "):app.index("const SFX_IDS")]
    if sound.count("new Audio(") != 1 or "el.loop = m.loop;" not in sound[sound.index("function startElement("):]:
        raise SystemExit("launcher/ui/app.js: an <audio> element plays music outside startElement (the fallback)")
    # (where the page reads the loop is where the launcher writes it: a smpl chunk's body starts 8 bytes after the chunk,
    # its first loop's start 44 bytes into the body and its last sample 48)
    need(app, "loop = { start: d.getUint32(body + 44, true), end: d.getUint32(body + 48, true) + 1 };",
         "launcher/ui/app.js: the smpl loop is read elsewhere")
    to_wav = bwav[bwav.index("public static byte[] ToWav(Pcm pcm)"):bwav.index("static (int Start, int End)? SmplLoop(")]
    for needle in ('Tag(at, "smpl"); Put(at + 4, 36 + 24, 4);', "Put(at + 36, 1, 4);", "Put(at + 52, loop.Start, 4); Put(at + 56, loop.End - 1, 4);",
                   "var at = 44 + data;", "Put(4, 36 + data + smpl, 4);"):
        need(to_wav, needle, "launcher/Core/GameFiles/Bwav.cs ToWav: the smpl chunk is written otherwise than the page reads it")
    need(bwav, "Loop = LoopOf(b, 0x10, frames)", "launcher/Core/GameFiles/Bwav.cs: Decode no longer reads the game's loop")
    need(bwav, "long end = U32(b, info + 0x3C), start = U32(b, info + 0x40);", "launcher/Core/GameFiles/Bwav.cs: the loop is read elsewhere in the header")
    # (the sounds' cache is made again once, with the loop; the art's files keep their names)
    wav = media[media.index("public string WavFile(string id)"):media.index("public IReadOnlyList<BntxTexture> Textures(")]
    need(wav, "}, SoundVersion);", "launcher/Core/GameFiles/GameMedia.cs: a WAV made before the loop was kept would be used")
    png = media[media.index("public string PngFile(string archive, string textureName)"):media.index("public string PngFile(string textureName)")]
    if "SoundVersion" in png:
        raise SystemExit("launcher/Core/GameFiles/GameMedia.cs: the art is decoded again for a change to the sounds")
    if not re.search(r'const string SoundVersion = CacheVersion \+ "\+loop-\d+";', media):
        raise SystemExit("launcher/Core/GameFiles/GameMedia.cs: SoundVersion is not CacheVersion and the loop's version")


def run_node(node, app):
    sound = app[app.index("const TRACKS = "):app.index("const SFX_IDS")]
    harness = read(ROOT / "tests" / "launcher_music_test.js")
    if harness.count("/*@@APP_SOUND@@*/") != 1:
        raise SystemExit("tests/launcher_music_test.js: no place for the page's code")
    with tempfile.TemporaryDirectory(prefix="launcher-music-") as folder:
        script = Path(folder) / "launcher_music_test.js"
        script.write_text(harness.replace("/*@@APP_SOUND@@*/", sound), encoding="utf-8")
        r = subprocess.run([node, str(script)], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)
    sys.stdout.write(r.stdout)
    if r.returncode != 0:
        raise SystemExit("the page's music in node failed:\n" + r.stdout + r.stderr)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    p.add_argument("--node", default="node")
    a = p.parse_args()
    node = shutil.which(a.node)
    if not node:
        p.error("node is needed to run the page's music code")
    app = read(LAUNCHER / "ui" / "app.js")
    check_source(app, read(LAUNCHER / "Core" / "GameFiles" / "Bwav.cs"), read(LAUNCHER / "Core" / "GameFiles" / "GameMedia.cs"))
    subprocess.run([node, "--check", str(LAUNCHER / "ui" / "app.js")], check=True)
    run_node(node, app)
    print("launcher music: source checks passed")


if __name__ == "__main__":
    main()
