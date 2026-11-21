# The launcher's music

The launcher plays the player's own game's music: Saria's Song on the reveal, the Temple of Time while it builds, the
item fanfare when the build is done, and the Fairy Fountain on the main screens (`MUSIC` in `launcher/ui/app.js`).
Nothing of it is in the repository or the exe: `launcher/Core/GameFiles` decodes the game's BWAV files into WAV files
in `%LOCALAPPDATA%\Triforce\launcher\media` (`<track>-<key>.wav`), and the page asks for them as `audio/<track>.wav`.

## The loop

Each of the three songs plays an intro once and then loops from a sample after it to its end, as in the game:

| Track | File | Loop |
|---|---|---|
| Saria's Song | the eight `BGM_LostWood_0` .. `_7` layers, mixed | after a short intro, to the end (every layer the same) |
| Fairy Fountain | `BGM_GreatFairyFountain` | after a long intro, to the end |
| Temple of Time | `BGM_ShrineOfTime_Child` | from a fraction of a second in, to the end |
| Item get | `BGM_ItemGet_Large` | none: it plays once |

The BWAV header gives the loop (each channel's end at 0x3C, 0xFFFFFFFF for none, and start at 0x40).
`Bwav.Decode` reads it, `Bwav.Mix` keeps it when every layer has the same one, and `Bwav.ToWav` writes it as a `smpl`
chunk after the samples: one forward loop, its last sample counted in, as the format has it. The samples stay at byte
44, so a reader that knows only the plain header still reads the file.

The page plays music through Web Audio: a track is fetched and decoded once into an `AudioBuffer`, and an
`AudioBufferSourceNode` loops it between `loopStart` and `loopEnd`, sample for sample (the context runs at the game's
48 kHz, so nothing is resampled). Its volume goes through a `GainNode` with the same fades, volume and mute as before. A
WAV without a `smpl` chunk (made by an older launcher) loops whole. Without Web Audio, or for a file it cannot decode,
an `<audio>` element plays it as before. The item fanfare starts at its full level (a fade-in would swallow its first
notes); a track faded out before it is read never starts, and a track that stops lets go of its decoded samples.

Before, an `<audio>` element looped the whole file: the intro came back on every loop, the end jumped to the start with
a step in the wave, and the element paused between the end and the start.

A launcher with this change decodes the sounds again once (the WAV cache's key has `SoundVersion` in it, and the files
made before are removed); the title art keeps its files.

## Tests

- `dotnet run --project launcher/LogicTests media`: the loop read from a BWAV the test writes, kept by the mix, written
  as `smpl` and read back, and the cache made again.
- `python -I -B tests/test_launcher_music.py`: the page's music code run in node with stand-ins for Web Audio, fetch and
  `<audio>` (`tests/launcher_music_test.js`), and the page and the launcher checked to read and write the loop at the
  same place.
