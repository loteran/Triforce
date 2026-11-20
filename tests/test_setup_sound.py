"""Triforce Setup plays the game's own decide and cancel sounds, from the player's game and from nowhere else
(setup/Sound.cs; no Setup is started, no game file is read, and no sound is made).

NX2navi's launcher played two of the game's interface sounds; Triforce Setup was silent. The sounds are the player's own
game's (Sound/Resource/Stream/SE_Sys_Do00.bwav and SE_Sys_Cancel00.bwav, or a mod's in their place), decoded in memory:
none is in the repository, a release or Setup.

Compiles Sound.cs (with Mods.cs and Guard.cs, no window) and tests/setup_sound_test.cs and runs them, on BWAV files the
test writes itself (PCM16, and DSP-ADPCM frames made by hand):

- the decoder: one channel to the left, to the right and in the middle, two channels, DSP-ADPCM with each kind of
  prediction, over several frames, held at 16 bits' ends; a file cut off after any number of its bytes, with samples
  said to lie past its end, another magic, no or too many channels, a codec that is neither, a count or a rate that
  is none, and thousands of damaged headers are refused or decoded, never by an exception;
- the WAV block handed to the player (its header's fields, its length), the loudest sample at the level's share of
  full scale, a sample past full scale held there;
- the setting's choices (Off, Low as the default, Medium, High: 0, 0.18, 0.40, 0.75), and what plays in which run:
  nothing in --snapshot, a script or a test folder;
- the file chosen: the game's own, the one of the last mod that is on and carries it, not a mod that is off or
  whose romfs folder is a link;
- playing, with a stand-in for the player: silence before a game is read, for a missing or undecodable file, with
  the setting Off, while the game runs, and for good once the PC cannot play; a mod's sound in place of the game's;
  the same block handed in again until the level or the files change.

Reads the source for the rest: the one place that makes a sound is Windows' PlaySound in WinPlayer, which only a
player's own run of Setup gets; nothing decoded is written to a file; the window plays on a click and on a choice
the player makes, and no sound file is in the repository.

    python -I -B tests/test_setup_sound.py
"""
import os
import sys
from pathlib import Path

RUN_ALL_SKIP = {"linux": "Triforce Setup is a Windows program (.NET Framework 4.8)"}

sys.path.insert(0, str(Path(__file__).resolve().parent))
import setup_cs  # noqa: E402


def code(rel):
    return "\n".join(line for line in setup_cs.source(rel).splitlines() if not line.lstrip().startswith("//"))


def check_source():
    sound = code("setup/Sound.cs")
    player = sound[sound.index("sealed class WinPlayer"):sound.index("sealed class NotedPlayer")]
    # (one place makes a sound, and one place makes that player: for a run that is nobody's test)
    if sound.count("PlaySound(") != player.count("PlaySound(") or "Memory | Async | NoDefault" not in player:
        raise SystemExit("setup/Sound.cs: a sound is played outside WinPlayer, or not from memory without waiting and without Windows' own ding")
    made = sound[sound.index("public static ISoundPlayer? PlayerFor("):sound.index("public static bool GameProcess()")]
    for needle in ("if (snapshot) return null;", "if (scripted || testRoot != null) return string.IsNullOrEmpty(noted) ? null : new NotedPlayer(noted!);", "return new WinPlayer();"):
        setup_cs.need(made, needle, "setup/Sound.cs PlayerFor: a run without a player may get one that plays (" + needle + ")")
    every = "".join(code("setup/" + f) for f in sorted(os.listdir(setup_cs.ROOT / "setup")) if f.endswith(".cs"))
    if every.count("new WinPlayer(") != 1 or every.count("winmm") != 1 or "System.Media" in every or "SystemSounds" in every or "Console.Beep" in every:
        raise SystemExit("setup/: a sound can be made past Sounds.PlayerFor")
    # (nothing decoded goes to the disk: the one file written is the test's note of what would have played)
    for call in ("File.Write", "FileStream(", "File.Create", "File.Copy", "File.Move", "File.Open"):
        if call in sound:
            raise SystemExit("setup/Sound.cs writes or opens a file with %s: the game's sounds stay in memory" % call)
    if sound.count("File.AppendAllText(") != 1 or "File.AppendAllText(" not in sound[sound.index("sealed class NotedPlayer"):sound.index("sealed class Sounds")]:
        raise SystemExit("setup/Sound.cs writes a file outside NotedPlayer")
    shell = setup_cs.source("setup/Shell.Sounds.cs")
    for needle, why in (
            ('Sounds.PlayerFor(snapshot != null, scripted, Install.TestRoot, Environment.GetEnvironmentVariable("TRIFORCE_SOUND_LOG"))',
             "the window no longer asks Sounds.PlayerFor who plays, with what kind of run this is"),
            ('var scripted = Environment.GetEnvironmentVariable("TRIFORCE_UI_SCRIPT") is { Length: > 0 };', "a scripted run is no longer known as one"),
            ("Window.AddHandler(ButtonBase.ClickEvent, new RoutedEventHandler(SoundOfClick), true);", "a click no longer sounds"),
            ("Window.AddHandler(Selector.SelectionChangedEvent, new SelectionChangedEventHandler(SoundOfChoice), true);", "a choice no longer sounds"),
            ("if (byHand && e.AddedItems.Count > 0 && e.OriginalSource is ListBox or ComboBox) sounds.Play(Sounds.Decide);", "a list the programme fills sounds, or a row the player picks does not"),
            ("button is RepeatButton || button.TemplatedParent != null) return;", "the parts of a scroll bar sound"),
            ("sounds.Play(GoBack.Contains(name) ? Sounds.Cancel : Sounds.Decide);", "going back no longer has the cancel sound"),
            ('Install.Set("Sounds", sounds.Level);', "the setting is no longer kept"),
            ('Install.Get("Sounds")', "the setting is no longer read"),
            ("Install.TestRoot != null ? new[] { \"Triforce\", \"hookshot\" }.Any(name => Running(name, Install.TestRoot, false)) : Sounds.GameProcess()", "Setup may sound over the running game")):
        setup_cs.need(shell, needle, "setup/Shell.Sounds.cs: " + why)
    back = shell[shell.index("static readonly HashSet<string> GoBack"):shell.index("void InitSounds()")]
    xaml = setup_cs.source("setup/Main.xaml")
    for name in ("SavesBack", "ModsBack", "SetupBack", "BackButton", "AskNo", "FetchCancel", "CancelButton"):
        setup_cs.need(back, '"%s"' % name, "setup/Shell.Sounds.cs: %s no longer has the cancel sound" % name)
        setup_cs.need(xaml, 'x:Name="%s"' % name, "setup/Main.xaml has no %s any more: setup/Shell.Sounds.cs GoBack names it" % name)
    setup_cs.need(xaml, 'x:Name="SoundsButton"', "setup/Main.xaml: the Sounds setting is gone")
    # (mouse movement makes no sound: nothing listens to it)
    for event in ("MouseMove", "MouseEnter", "GotFocus", "IsMouseOver"):
        if event in shell:
            raise SystemExit("setup/Shell.Sounds.cs listens to %s: moving over things makes no sound" % event)
    build = setup_cs.source("setup/build.ps1")
    for file in ("Sound.cs", "Shell.Sounds.cs"):
        setup_cs.need(build, '"$here\\%s"' % file, "setup/build.ps1 no longer compiles " + file)
    # (no sound is shipped: none in the folders a release is made from, nor among the tests)
    for folder in ("setup", "assets", "tests", "nx2", "host"):
        for base, dirs, files in os.walk(setup_cs.ROOT / folder):
            dirs[:] = [d for d in dirs if d not in ("out", "__pycache__")]
            for f in files:
                if f.lower().endswith((".wav", ".bwav", ".bfwav", ".bfstm", ".ogg", ".mp3", ".flac", ".opus")):
                    raise SystemExit("a sound file is in the repository: %s (Setup plays the player's own game's, and ships none)" % os.path.join(base, f))


def main():
    check_source()
    setup_cs.run_fixture("setup_sound_test.cs", sources=("setup/Mods.cs", "setup/Sound.cs"))


if __name__ == "__main__":
    main()
