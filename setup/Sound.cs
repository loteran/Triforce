// (Triforce) Setup's sounds: two of the game's own interface sounds, the decide sound and the cancel sound of its menus, read
// from the player's built game and played when a button, link or row is chosen and when the player goes back (Shell.Sounds.cs
// says where). Nothing here needs the window: tests/test_setup_sound.py compiles this file with Mods.cs and Guard.cs and runs
// it on sound files it authors itself.
//
// No sound is in the repository, in a release or in Setup, and Setup makes none of its own: until a game is built, and for
// a file that is missing or does not decode, it is silent, without a word. The two files are decoded in memory and stay
// there: nothing made from the game's audio is written to the disk.
//
//   <game>\romfs\Sound\Resource\Stream\SE_Sys_Do00.bwav       decide
//   <game>\romfs\Sound\Resource\Stream\SE_Sys_Cancel00.bwav   cancel
//   <game>\romfs\Sound\Resource\Stream\SE_Sys_SysMenu_DecideL00.bwav   start ((Triforce, 11-17) Play: the sound of the game's Load
//   list when a save is chosen)
// or the file a mod puts in their place, by the game's own rule (Mods.RomfsFile): the game would play the mod's.
//
// BWAV (from NX2navi's runtime/menu_sound.c, the owner's, as is the decoder below): "BWAV", a byte order mark, a version, a
// CRC32, a prefetch flag (u16) and the channel count (u16 at 0x0E), then 0x4C bytes for each channel: codec (u16: 0 PCM16,
// 1 DSP-ADPCM), pan (u16: 0 left, 1 right, 2 middle), sample rate (u32), sample count (u32), the count again, 16 DSP
// coefficients (s16), the place of the samples in the file (u32 at 0x30), ... DSP-ADPCM is frames of 8 bytes: a header (the
// coefficient pair's index, the scale's exponent) and 14 samples of four bits. The interface sounds are 48 kHz DSP-ADPCM in
// two channels. All numbers little-endian.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;

namespace TriforceSetup;

/// A decoded sound: left and right in turn, -1 to 1
sealed class Clip
{
    public int Rate;
    public float[] Samples = { };
    public int Frames => Samples.Length / 2;
}

static class Bwav
{
    /// How long a sound may be, in seconds (the interface sounds last about one): a count beyond it is no such file
    public const int MaxSeconds = 10;

    static int U16(byte[] b, long at) => b[at] | b[at + 1] << 8;
    static long U32(byte[] b, long at) => b[at] | (long)b[at + 1] << 8 | (long)b[at + 2] << 16 | (long)b[at + 3] << 24;

    /// The file decoded, or null when it is no BWAV file of one or two channels of PCM16 or DSP-ADPCM that is whole. Every
    /// place read is asked against the file's length first: a file cut off or with numbers that point past its end is refused
    /// here, not by an exception
    public static Clip? Decode(byte[]? b)
    {
        if (b == null || b.Length < 0x10 || b[0] != 'B' || b[1] != 'W' || b[2] != 'A' || b[3] != 'V') return null;
        var channels = U16(b, 0x0E);
        if (channels < 1 || channels > 2 || 0x10 + channels * 0x4C > b.Length) return null;
        long rate = U32(b, 0x10 + 4), frames = U32(b, 0x10 + 8);
        if (rate < 8000 || rate > 96000 || frames == 0 || frames > rate * MaxSeconds) return null;
        // (first whether every channel's samples are in the file, then the room for them: a file that only says it is long gets none)
        for (var c = 0; c < channels; c++)
            if (!Whole(b, 0x10 + c * 0x4C, frames)) return null;
        var pcm = new float[frames * 2];
        for (var c = 0; c < channels; c++) Channel(b, 0x10 + c * 0x4C, pcm, frames);
        // (one channel, in the middle: in both, at the level that sounds as loud)
        if (channels == 1)
            for (var i = 0; i < pcm.Length; i++) pcm[i] *= 0.7071f;
        return new Clip { Rate = (int)rate, Samples = pcm };
    }

    /// Is the channel one of the two codecs, with all of its samples (as many as the sound has, at most) inside the file?
    static bool Whole(byte[] b, long info, long frames)
    {
        var codec = U16(b, info);
        long count = Math.Min(U32(b, info + 8), frames), at = U32(b, info + 0x30);
        return codec == 0 ? at + count * 2 <= b.Length : codec == 1 && at + (count + 13) / 14 * 8 <= b.Length;
    }

    /// One channel (Whole has looked at it) added into its side: pan 0 left, 1 right, anything else both
    static void Channel(byte[] b, long info, float[] into, long frames)
    {
        int codec = U16(b, info), pan = U16(b, info + 2);
        long count = Math.Min(U32(b, info + 8), frames), at = U32(b, info + 0x30);
        const float scale = 1f / 32768f;
        if (codec == 0)
        {
            for (long i = 0; i < count; i++)
            {
                var v = (short)U16(b, at + 2 * i) * scale;
                if (pan != 1) into[2 * i] += v;
                if (pan != 0) into[2 * i + 1] += v;
            }
            return;
        }
        var pairs = new int[16];
        for (var i = 0; i < 16; i++) pairs[i] = (short)U16(b, info + 0x10 + 2 * i);
        long h1 = 0, h2 = 0;
        for (long i = 0; i < count; i++)
        {
            var frame = at + i / 14 * 8;
            var k = (int)(i % 14);
            int index = b[frame] >> 4 & 7, shift = b[frame] & 15;
            var nibble = b[frame + 1 + k / 2] >> ((k & 1) != 0 ? 0 : 4) & 15;
            if (nibble >= 8) nibble -= 16;
            var s = ((long)(nibble << shift) << 11) + pairs[2 * index] * h1 + pairs[2 * index + 1] * h2;
            var v = Math.Max(-32768, Math.Min(32767, (s + 1024) >> 11));
            h2 = h1;
            h1 = v;
            if (pan != 1) into[2 * i] += v * scale;
            if (pan != 0) into[2 * i + 1] += v * scale;
        }
    }
}

/// What plays a sound. (The window's is WinPlayer; a test puts its own here, and a test run of Setup none at all.)
interface ISoundPlayer
{
    /// Starts a WAV file's bytes, in place of whatever plays. The same array is handed in again for the same sound
    void Play(string what, byte[] wav);
    /// The arrays handed in so far will not come again: whatever plays is stopped
    void Forget();
}

/// Windows' own PlaySound, from memory, without waiting for the sound to end and without Windows' "ding" where a sound cannot
/// be played. It reads the bytes while it plays, so each array stays pinned in its place until it is forgotten, and then one
/// forgetting longer
sealed class WinPlayer : ISoundPlayer
{
    [DllImport("winmm.dll", EntryPoint = "PlaySoundW", SetLastError = true)]
    static extern bool PlaySound(IntPtr sound, IntPtr module, uint flags);
    const uint Async = 0x0001, NoDefault = 0x0002, Memory = 0x0004;

    readonly Dictionary<byte[], GCHandle> held = new();
    List<GCHandle> let = new();

    public void Play(string what, byte[] wav)
    {
        if (!held.TryGetValue(wav, out var pinned)) held[wav] = pinned = GCHandle.Alloc(wav, GCHandleType.Pinned);
        PlaySound(pinned.AddrOfPinnedObject(), IntPtr.Zero, Memory | Async | NoDefault);
    }

    public void Forget()
    {
        try { PlaySound(IntPtr.Zero, IntPtr.Zero, 0); } catch (Exception) { }
        foreach (var h in let) h.Free();
        let = held.Values.ToList();
        held.Clear();
    }
}

/// (testing, TRIFORCE_SOUND_LOG=<file> in a test run of Setup: no sound is made; each one that would have played is a line
/// in that file, with the size of its WAV block and its loudest sample)
sealed class NotedPlayer : ISoundPlayer
{
    readonly string file;
    public NotedPlayer(string file) { this.file = file; }

    public void Play(string what, byte[] wav)
    {
        var peak = 0;
        for (var i = 44; i + 1 < wav.Length; i += 2) peak = Math.Max(peak, Math.Abs((int)(short)(wav[i] | wav[i + 1] << 8)));
        try { File.AppendAllText(file, what + " " + wav.Length + " bytes, peak " + peak + Environment.NewLine); } catch (Exception) { }
    }

    public void Forget() { }
}

sealed class Sounds
{
    public const string Decide = "decide", Cancel = "cancel", Start = "start";
    /// The game's files, as its RomFS names them
    public static readonly (string Sound, string File)[] Files =
    {
        (Decide, "Sound/Resource/Stream/SE_Sys_Do00.bwav"), (Cancel, "Sound/Resource/Stream/SE_Sys_Cancel00.bwav"),
        (Start, "Sound/Resource/Stream/SE_Sys_SysMenu_DecideL00.bwav"),
    };
    /// The setting's choices: the loudest sample's share of full scale (NX2navi's launcher's levels)
    public static readonly (string Name, double Share)[] Levels = { ("off", 0), ("low", 0.18), ("medium", 0.40), ("high", 0.75) };
    public const string Default = "low";
    /// The most of a sound file that is read (the game's two are 63 KB and 50 KB)
    public const int MaxFile = 4 << 20;

    /// A setting's value as one of the choices: the default for none, or for one that is no choice
    public static string Known(string? level) => Levels.Any(l => l.Name == level) ? level! : Default;
    public static double Share(string? level) => Levels.First(l => l.Name == Known(level)).Share;
    /// The choice after this one, around: off, low, medium, high, off
    public static string Next(string? level) => Levels[(Array.FindIndex(Levels, l => l.Name == Known(level)) + 1) % Levels.Length].Name;

    /// What plays the sounds of this run of Setup. Nothing in a run that only looks at Setup or drives it without a person:
    /// --snapshot, a script (TRIFORCE_UI_SCRIPT) or a test folder (TRIFORCE_TEST_ROOT). Such a run, when it asks (noted: the
    /// file of TRIFORCE_SOUND_LOG), gets the sounds written down instead of played; --snapshot not even that
    public static ISoundPlayer? PlayerFor(bool snapshot, bool scripted, string? testRoot, string? noted)
    {
        if (snapshot) return null;
        if (scripted || testRoot != null) return string.IsNullOrEmpty(noted) ? null : new NotedPlayer(noted!);
        return new WinPlayer();
    }

    /// Does the game run, any game folder's? Its own sound is playing then, and Setup's would play over it
    public static bool GameProcess()
    {
        try
        {
            foreach (var name in new[] { "Triforce", "hookshot" })
            {
                var found = Process.GetProcessesByName(name);
                foreach (var p in found) p.Dispose();
                if (found.Length > 0) return true;
            }
        }
        catch (Exception) { }
        return false;
    }

    /// The sound as a WAV file's bytes, 16 bits in two channels, with its loudest sample at that share of full scale; a
    /// sample that would pass full scale stays at it. Null for a sound that is all silence
    public static byte[]? Wav(Clip clip, double share)
    {
        var peak = 0f;
        foreach (var v in clip.Samples) peak = Math.Max(peak, Math.Abs(v));
        if (peak <= 0 || clip.Samples.Length == 0) return null;
        var data = clip.Samples.Length * 2;
        var wav = new byte[44 + data];
        void Tag(int at, string tag) { for (var i = 0; i < 4; i++) wav[at + i] = (byte)tag[i]; }
        void Put(int at, int value, int bytes) { for (var i = 0; i < bytes; i++) wav[at + i] = (byte)(value >> 8 * i); }
        Tag(0, "RIFF"); Put(4, 36 + data, 4); Tag(8, "WAVE");
        Tag(12, "fmt "); Put(16, 16, 4); Put(20, 1, 2); Put(22, 2, 2); Put(24, clip.Rate, 4); Put(28, clip.Rate * 4, 4); Put(32, 4, 2); Put(34, 16, 2);
        Tag(36, "data"); Put(40, data, 4);
        var gain = share / peak * 32767;
        for (var i = 0; i < clip.Samples.Length; i++)
            Put(44 + 2 * i, (int)Math.Max(-32767, Math.Min(32767, Math.Round(clip.Samples[i] * gain))), 2);
        return wav;
    }

    readonly ISoundPlayer? player;
    readonly Func<bool> gameRuns;
    readonly object gate = new();
    readonly Dictionary<string, Clip> clips = new();
    readonly Dictionary<string, byte[]> wavs = new();
    string level = Default, from = "";
    bool broken;

    /// player: null for a Setup that makes no sound at all (then no file is read either). gameRuns: asked before each sound
    public Sounds(ISoundPlayer? player, Func<bool> gameRuns, string? level = null)
    {
        this.player = player;
        this.gameRuns = gameRuns;
        this.level = Known(level);
    }

    /// The setting. Changing it stops what plays
    public string Level
    {
        get { lock (gate) return level; }
        set
        {
            lock (gate)
            {
                if (Known(value) == level) return;
                level = Known(value);
                Renew();
            }
        }
    }

    /// The WAV blocks anew, for the sounds and the level there are now
    void Renew()
    {
        try { player?.Forget(); } catch (Exception) { broken = true; }
        wavs.Clear();
        var share = Share(level);
        if (share <= 0) return;
        foreach (var c in clips)
            if (Wav(c.Value, share) is { } wav) wavs[c.Key] = wav;
    }

    /// Takes up the two sounds of a game folder (null: no game yet), from its files or a mod's. Again whenever the game folder
    /// or its mods may have changed: nothing is read again while the same files, by size and time, are the ones to play.
    /// Whatever cannot be read or decoded is no sound: nothing is said, nothing thrown
    public void Load(string? game)
    {
        if (player == null) return;
        try
        {
            var chosen = new List<(string Sound, string? File, string Mark)>();
            foreach (var (sound, rel) in Files)
            {
                var file = game != null ? Mods.RomfsFile(game, rel) : null;
                var mark = "";
                if (file != null)
                {
                    var info = new FileInfo(file);
                    mark = file + "|" + info.Length + "|" + info.LastWriteTimeUtc.Ticks;
                }
                chosen.Add((sound, file, mark));
            }
            var all = string.Join("\n", chosen.Select(c => c.Mark));
            lock (gate)
                if (all == from) return;
            var read = new Dictionary<string, Clip>();
            foreach (var (sound, file, _) in chosen)
            {
                if (file == null || new FileInfo(file).Length > MaxFile) continue;
                if (Bwav.Decode(File.ReadAllBytes(file)) is { } clip) read[sound] = clip;
            }
            lock (gate)
            {
                from = all;
                clips.Clear();
                foreach (var c in read) clips[c.Key] = c.Value;
                Renew();
            }
        }
        catch (Exception) { }
    }

    /// Plays one of the two, in place of whatever plays. Silent when the setting is Off, the game has no such sound, the game
    /// runs, or this Setup makes no sound; and from the first time the PC cannot play one (no sound device), for good
    /// How long a sound lasts, in seconds (0: none to play)
    public double Seconds(string sound)
    {
        lock (gate) return clips.TryGetValue(sound, out var c) && c.Rate > 0 && wavs.ContainsKey(sound) ? (double)c.Frames / c.Rate : 0;
    }

    public void Play(string sound)
    {
        if (player == null) return;
        lock (gate)
        {
            if (broken || !wavs.TryGetValue(sound, out var wav)) return;
            try
            {
                if (gameRuns()) return;
                player.Play(sound, wav);
            }
            catch (Exception) { broken = true; }
        }
    }
}
