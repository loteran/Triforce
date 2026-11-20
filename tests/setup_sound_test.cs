// tests/test_setup_sound.py: setup/Sound.cs (compiled with Mods.cs and Guard.cs, no window) on sound files this program
// authors itself, in a game folder it makes in a temporary folder. No game file is read, no Setup runs, and no sound is
// made: what plays is a stand-in that writes down what it was handed.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace TriforceSetup;

static class SoundTest
{
    static int checks;

    static void Check(bool ok, string what)
    {
        if (!ok) throw new Exception(what);
        checks++;
    }

    // ---- BWAV files, written here ----

    sealed class Channel
    {
        public int Codec, Pan, Count;
        public byte[] Data = { };
        public short[] Pairs = new short[16];
    }

    static Channel Pcm(int pan, params short[] samples) =>
        new() { Codec = 0, Pan = pan, Count = samples.Length, Data = samples.SelectMany(s => BitConverter.GetBytes(s)).ToArray() };

    /// DSP-ADPCM frames by hand: for each frame its coefficient pair, its scale's exponent, and up to 14 samples of -8..7
    static Channel Adpcm(int pan, short[] pairs, int count, params (int Pair, int Shift, int[] Nibbles)[] frames)
    {
        var data = new List<byte>();
        foreach (var (pair, shift, nibbles) in frames)
        {
            var frame = new byte[8];
            frame[0] = (byte)(pair << 4 | shift);
            for (var k = 0; k < nibbles.Length; k++) frame[1 + k / 2] |= (byte)((nibbles[k] & 15) << ((k & 1) != 0 ? 0 : 4));
            data.AddRange(frame);
        }
        var c = new Channel { Codec = 1, Pan = pan, Count = count, Data = data.ToArray() };
        pairs.CopyTo(c.Pairs, 0);
        return c;
    }

    static void U16(byte[] b, int at, int v) { b[at] = (byte)v; b[at + 1] = (byte)(v >> 8); }
    static void U32(byte[] b, int at, long v) { for (var i = 0; i < 4; i++) b[at + i] = (byte)(v >> 8 * i); }

    /// The file: the header, 0x4C bytes for each channel, then each channel's samples at the next multiple of 0x40
    static byte[] File_(int rate, params Channel[] channels)
    {
        var at = 0x10 + channels.Length * 0x4C;
        var places = new List<int>();
        foreach (var c in channels)
        {
            at = (at + 0x3F) / 0x40 * 0x40;
            places.Add(at);
            at += c.Data.Length;
        }
        var b = new byte[at];
        Encoding.ASCII.GetBytes("BWAV").CopyTo(b, 0);
        U16(b, 4, 0xFEFF);
        U16(b, 6, 1);
        U16(b, 0x0E, channels.Length);
        for (var i = 0; i < channels.Length; i++)
        {
            var (c, info) = (channels[i], 0x10 + i * 0x4C);
            U16(b, info, c.Codec);
            U16(b, info + 2, c.Pan);
            U32(b, info + 4, rate);
            U32(b, info + 8, c.Count);
            U32(b, info + 0x0C, c.Count);
            for (var k = 0; k < 16; k++) U16(b, info + 0x10 + 2 * k, c.Pairs[k]);
            U32(b, info + 0x30, places[i]);
            U32(b, info + 0x34, places[i]);
            U32(b, info + 0x38, 1);
            U32(b, info + 0x3C, 0xFFFFFFFF);
            c.Data.CopyTo(b, places[i]);
        }
        return b;
    }

    static bool Near(float a, double b) => Math.Abs(a - b) < 1e-6;

    static bool Same(Clip? clip, double[] left, double[] right) =>
        clip != null && clip.Frames == left.Length && Enumerable.Range(0, left.Length).All(i => Near(clip.Samples[2 * i], left[i]) && Near(clip.Samples[2 * i + 1], right[i]));

    static double[] Of(double scale, params int[] values) => values.Select(v => v * scale).ToArray();

    /// What plays, in the test: it writes down what it was handed
    sealed class Noting : ISoundPlayer
    {
        public readonly List<(string What, byte[] Wav)> Played = new();
        public int Forgot;
        public bool Fails;
        public void Play(string what, byte[] wav)
        {
            if (Fails) throw new InvalidOperationException("no sound device");
            Played.Add((what, wav));
        }
        public void Forget() => Forgot++;
    }

    static int Peak(byte[] wav)
    {
        var peak = 0;
        for (var i = 44; i < wav.Length; i += 2) peak = Math.Max(peak, Math.Abs((int)BitConverter.ToInt16(wav, i)));
        return peak;
    }

    static int Main()
    {
        Decoder();
        Refused();
        Wav();
        Levels();
        var temp = Path.Combine(Path.GetTempPath(), "triforce-sound-test-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temp);
        try
        {
            Chosen(Path.Combine(temp, "choice"));
            Playing(Path.Combine(temp, "playing"));
        }
        finally
        {
            try { Directory.Delete(temp, true); } catch (Exception) { }
        }
        Console.WriteLine("setup_sound_test: " + checks + " checks passed");
        return 0;
    }

    const double S = 1.0 / 32768, Half = 0.7071;

    static void Decoder()
    {
        // PCM16, one channel: to the left, to the right, and in the middle (both sides, at 0.7071)
        short[] pcm = { 0, 1000, -1000, 32767, -32768, 12345 };
        var values = pcm.Select(v => (int)v).ToArray();
        var none = new double[pcm.Length];
        Check(Same(Bwav.Decode(File_(48000, Pcm(0, pcm))), Of(S * Half, values), none), "PCM16 to the left");
        Check(Same(Bwav.Decode(File_(48000, Pcm(1, pcm))), none, Of(S * Half, values)), "PCM16 to the right");
        Check(Same(Bwav.Decode(File_(48000, Pcm(2, pcm))), Of(S * Half, values), Of(S * Half, values)), "PCM16 in the middle");
        var clip = Bwav.Decode(File_(44100, Pcm(2, pcm)));
        Check(clip != null && clip.Rate == 44100 && clip.Frames == 6 && clip.Samples.Length == 12, "the rate and the length of a decoded sound");
        // two channels: each on its side, as it is
        short[] other = { 5, -5, 50, -50, 500, -500 };
        Check(Same(Bwav.Decode(File_(48000, Pcm(0, pcm), Pcm(1, other))), Of(S, values), Of(S, other.Select(v => (int)v).ToArray())), "PCM16 in two channels");
        // (a second channel with fewer samples than the first: silence after its end; with more: no further than the first)
        Check(Same(Bwav.Decode(File_(48000, Pcm(0, pcm), Pcm(1, 7, 8))), Of(S, values), Of(S, 7, 8, 0, 0, 0, 0)), "a shorter second channel");
        Check(Same(Bwav.Decode(File_(48000, Pcm(0, 7, 8), Pcm(1, other))), Of(S, 7, 8), Of(S, 5, -5)), "a longer second channel");

        // DSP-ADPCM by hand. A pair of (0, 0): each sample is its four bits times two to the scale's exponent
        short[] pairs = { 0, 0, 2048, 0, 4096, -2048 };
        var plain = Adpcm(0, pairs, 6, (0, 3, new[] { 1, -1, 7, -8, 0, 3 }));
        Check(Same(Bwav.Decode(File_(48000, plain, Pcm(1, 0, 0, 0, 0, 0, 0))), Of(S, 8, -8, 56, -64, 0, 24), new double[6]), "DSP-ADPCM, no prediction");
        // (2048, 0): the sample before is added, a running sum; and it stays at 16 bits' ends
        var sum = Adpcm(2, pairs, 5, (1, 0, new[] { 1, 1, 1, -4, 2 }));
        Check(Same(Bwav.Decode(File_(48000, sum)), Of(S * Half, 1, 2, 3, -1, 1), Of(S * Half, 1, 2, 3, -1, 1)), "DSP-ADPCM, the sample before added");
        var loud = Adpcm(0, pairs, 4, (1, 12, new[] { 7, 7, -8, -8 }));
        Check(Same(Bwav.Decode(File_(48000, loud, Pcm(1, 0, 0, 0, 0))), Of(S, 28672, 32767, -1, -32768), new double[4]), "DSP-ADPCM held at 16 bits' ends");
        // (4096, -2048): twice the sample before less the one before that
        var two = Adpcm(1, pairs, 5, (2, 0, new[] { 1, 1, 1, 0, -2 }));
        Check(Same(Bwav.Decode(File_(48000, Pcm(0, 0, 0, 0, 0, 0), two)), new double[5], Of(S, 1, 3, 6, 9, 10)), "DSP-ADPCM, two samples before");
        // more than one frame, the last one not full, the pair and the scale changing from frame to frame; in two channels
        var first = Enumerable.Range(0, 14).Select(i => i % 8).ToArray();
        var longer = Adpcm(0, pairs, 17, (0, 1, first), (1, 2, new[] { 1, -1, 2 }));
        var expect = first.Select(n => n * 2).Concat(new[] { 10 + 4, 10 + 4 - 4, 10 + 4 - 4 + 8 }).ToArray();   // (the first frame ends at 5 * 2)
        var right = Adpcm(1, pairs, 17, (0, 0, first), (0, 4, new[] { -1, 0, 1 }));
        Check(Same(Bwav.Decode(File_(48000, longer, right)), Of(S, expect), Of(S, first.Concat(new[] { -16, 0, 16 }).ToArray())), "DSP-ADPCM over two frames, in two channels");
        // (the pair's index is three bits: the bit above it is not part of it)
        var high = Adpcm(0, pairs, 2, (8 | 1, 0, new[] { 3, 3 }));
        Check(Same(Bwav.Decode(File_(48000, high, Pcm(1, 0, 0))), Of(S, 3, 6), new double[2]), "DSP-ADPCM, the bit above the pair's index");
    }

    static void Refused()
    {
        short[] pairs = { 0, 0, 2048, 0 };
        var first = Enumerable.Range(0, 14).Select(i => i % 8 - 4).ToArray();
        var files = new[]
        {
            File_(48000, Pcm(2, 1, 2, 3, 4, 5)), File_(48000, Pcm(0, 1, 2, 3), Pcm(1, 4, 5, 6)),
            File_(48000, Adpcm(2, pairs, 20, (1, 2, first), (0, 1, new[] { 1, 2, 3, 4, 5, 6 }))),
            File_(48000, Adpcm(0, pairs, 15, (1, 2, first), (0, 1, new[] { 1 })), Adpcm(1, pairs, 15, (0, 0, first), (1, 1, new[] { 2 }))),
        };
        Check(Bwav.Decode(null) == null && Bwav.Decode(new byte[0]) == null && Bwav.Decode(new byte[0x0F]) == null, "no file at all was decoded");
        foreach (var whole in files)
        {
            Check(Bwav.Decode(whole) != null, "(the test) a whole file is refused");
            // cut off anywhere: refused, by the look at its length and not by an exception (one would end this program)
            for (var length = 0; length < whole.Length; length++)
                Check(Bwav.Decode(whole.Take(length).ToArray()) == null, "a file cut off after " + length + " of " + whole.Length + " bytes was decoded");
            // (bytes after its end do not matter)
            Check(Bwav.Decode(whole.Concat(new byte[100]).ToArray()) != null, "a file with bytes after its samples was refused");
            // another magic, no channel, three, 65535
            foreach (var (at, value) in new[] { (0, (int)'R'), (3, (int)'E'), (0x0E, 0), (0x0E, 3), (0x0F, 0xFF) })
            {
                var bad = (byte[])whole.Clone();
                bad[at] = (byte)value;
                Check(Bwav.Decode(bad) == null, "a file with " + value + " at " + at + " was decoded");
            }
            var channels = whole[0x0E];
            for (var c = 0; c < channels; c++)
            {
                var info = 0x10 + c * 0x4C;
                // samples that would lie past the file's end, wherever they are said to begin; and a codec that is neither
                foreach (var place in new long[] { whole.Length, whole.Length - 1, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, whole.Length - (c == channels - 1 ? 1 : 0) + 100 })
                {
                    var bad = (byte[])whole.Clone();
                    U32(bad, info + 0x30, place);
                    Check(Bwav.Decode(bad) == null, "samples said to begin at " + place + " in a file of " + whole.Length + " bytes were decoded");
                }
                foreach (var codec in new[] { 2, 3, 0xFFFF })
                {
                    var bad = (byte[])whole.Clone();
                    U16(bad, info, codec);
                    Check(Bwav.Decode(bad) == null, "codec " + codec + " was decoded");
                }
            }
            // a count the file does not hold, none, or more than a sound has; a rate that is none
            foreach (var count in new long[] { 0, 100000, 480001, 0x7FFFFFFF, 0xFFFFFFFF })
            {
                var bad = (byte[])whole.Clone();
                U32(bad, 0x18, count);
                Check(Bwav.Decode(bad) == null, "a file that says it has " + count + " samples was decoded");
            }
            foreach (var rate in new long[] { 0, 1, 7999, 96001, 0xFFFFFFFF })
            {
                var bad = (byte[])whole.Clone();
                U32(bad, 0x14, rate);
                Check(Bwav.Decode(bad) == null, "a file at " + rate + " Hz was decoded");
            }
            // every byte of the header as 00, 7F, 80 and FF in turn, then many at once at random: decoded or refused, and
            // nothing thrown (an exception would end this program)
            var header = 0x10 + channels * 0x4C;
            for (var at = 0; at < header; at++)
                foreach (var value in new byte[] { 0x00, 0x7F, 0x80, 0xFF })
                {
                    var bad = (byte[])whole.Clone();
                    bad[at] = value;
                    Bwav.Decode(bad);
                    checks++;
                }
            var random = new Random(20261115);
            for (var n = 0; n < 4000; n++)
            {
                var bad = (byte[])whole.Clone();
                for (var k = random.Next(1, 8); k > 0; k--) bad[random.Next(4, header)] = (byte)random.Next(256);
                var clip = Bwav.Decode(n % 2 == 0 ? bad : bad.Take(random.Next(header, bad.Length + 1)).ToArray());
                Check(clip == null || clip.Samples.Length == clip.Frames * 2 && clip.Frames <= 96000 * Bwav.MaxSeconds, "a damaged file decoded to something that is no sound");
            }
        }
    }

    static void Wav()
    {
        var clip = new Clip { Rate = 48000, Samples = new[] { 0f, 0.25f, -0.5f, 0.1f, 0.5f, -0.25f } };
        var wav = Sounds.Wav(clip, 0.5)!;
        // the block handed to the player: a WAV file of 16 bits in two channels at the sound's rate
        Check(wav.Length == 44 + 12 && Encoding.ASCII.GetString(wav, 0, 4) == "RIFF" && BitConverter.ToInt32(wav, 4) == wav.Length - 8 && Encoding.ASCII.GetString(wav, 8, 8) == "WAVEfmt ", "the WAV block's beginning");
        Check(BitConverter.ToInt32(wav, 16) == 16 && BitConverter.ToInt16(wav, 20) == 1 && BitConverter.ToInt16(wav, 22) == 2 && BitConverter.ToInt32(wav, 24) == 48000 &&
              BitConverter.ToInt32(wav, 28) == 48000 * 4 && BitConverter.ToInt16(wav, 32) == 4 && BitConverter.ToInt16(wav, 34) == 16, "the WAV block's format");
        Check(Encoding.ASCII.GetString(wav, 36, 4) == "data" && BitConverter.ToInt32(wav, 40) == 12, "the WAV block's data");
        // the loudest sample at the level's share of full scale, the others in proportion
        var samples = Enumerable.Range(0, 6).Select(i => (int)BitConverter.ToInt16(wav, 44 + 2 * i)).ToArray();
        Check(samples.SequenceEqual(new[] { 0, 8192, -16384, 3277, 16384, -8192 }), "the samples at half of full scale: " + string.Join(" ", samples));
        Check(Peak(Sounds.Wav(clip, 1)!) == 32767 && Peak(Sounds.Wav(clip, 0.18)!) == 5898, "the loudest sample at the level");
        // (a quiet sound is as loud as a loud one: the level is the loudest sample's)
        var quiet = new Clip { Rate = 22050, Samples = new[] { 0.001f, -0.002f } };
        var q = Sounds.Wav(quiet, 0.4)!;
        Check(BitConverter.ToInt16(q, 44) == 6553 && BitConverter.ToInt16(q, 46) == -13107 && BitConverter.ToInt32(q, 24) == 22050 && BitConverter.ToInt32(q, 28) == 88200, "a quiet sound at the level");
        // a level past full scale: held at it, not wrapped around to the other sign
        var over = Sounds.Wav(clip, 3)!;
        samples = Enumerable.Range(0, 6).Select(i => (int)BitConverter.ToInt16(over, 44 + 2 * i)).ToArray();
        Check(samples.SequenceEqual(new[] { 0, 32767, -32767, 19660, 32767, -32767 }), "samples past full scale: " + string.Join(" ", samples));
        Check(Sounds.Wav(new Clip { Rate = 48000, Samples = new float[8] }, 0.5) == null && Sounds.Wav(new Clip { Rate = 48000 }, 0.5) == null, "silence became a sound");
    }

    static void Levels()
    {
        Check(string.Join(" ", Sounds.Levels.Select(l => l.Name + "=" + l.Share.ToString(System.Globalization.CultureInfo.InvariantCulture))) == "off=0 low=0.18 medium=0.4 high=0.75", "the levels");
        Check(Sounds.Default == "low" && Sounds.Known(null) == "low" && Sounds.Known("") == "low" && Sounds.Known("loud") == "low" && Sounds.Known("Off") == "low" && Sounds.Known("off") == "off" && Sounds.Known("high") == "high",
              "a setting that is none of the choices is not the default");
        Check(Sounds.Share("off") == 0 && Sounds.Share(null) == 0.18 && Sounds.Share("medium") == 0.40, "a level's share of full scale");
        Check(Sounds.Next("off") == "low" && Sounds.Next("low") == "medium" && Sounds.Next("medium") == "high" && Sounds.Next("high") == "off" && Sounds.Next(null) == "medium", "the choice after each choice");
        Check(Sounds.Files.Length == 3 && Sounds.Files[0] == ("decide", "Sound/Resource/Stream/SE_Sys_Do00.bwav") && Sounds.Files[1] == ("cancel", "Sound/Resource/Stream/SE_Sys_Cancel00.bwav") && Sounds.Files[2] == ("start", "Sound/Resource/Stream/SE_Sys_SysMenu_DecideL00.bwav"), "the game's three files (Play's start sound, 11-17)");
        // who plays: nobody in a run without a player; such a run that asks gets the sounds written down; --snapshot not even that
        Check(Sounds.PlayerFor(false, false, null, null) is WinPlayer && Sounds.PlayerFor(false, false, null, @"C:\log.txt") is WinPlayer, "a player's Setup has no sound");
        Check(Sounds.PlayerFor(true, false, null, null) == null && Sounds.PlayerFor(false, true, null, null) == null && Sounds.PlayerFor(false, false, @"C:\test", null) == null &&
              Sounds.PlayerFor(false, true, @"C:\test", "") == null, "a run without a player has a sound player");
        Check(Sounds.PlayerFor(false, true, null, @"C:\log.txt") is NotedPlayer && Sounds.PlayerFor(false, false, @"C:\test", @"C:\log.txt") is NotedPlayer && Sounds.PlayerFor(true, true, @"C:\test", @"C:\log.txt") == null,
              "a test run that asks for its sounds does not get them written down, or --snapshot does");
    }

    static void Put(string file, byte[] bytes)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(file)!);
        File.WriteAllBytes(file, bytes);
    }

    const string Do = "Sound/Resource/Stream/SE_Sys_Do00.bwav";

    /// The same file? (Windows spells a path in the case it was asked for, whatever the folder's own is)
    static bool Is(string? a, string b) => string.Equals(a, b, StringComparison.OrdinalIgnoreCase);

    static void Chosen(string game)
    {
        string In(params string[] parts) => Path.Combine(new[] { game }.Concat(parts).ToArray());
        var sound = File_(48000, Pcm(2, 1, 2, 3));
        Check(Mods.RomfsFile(game, Do) == null, "a game folder that is not there has a sound");
        Directory.CreateDirectory(game);
        Check(Mods.RomfsFile(game, Do) == null, "an empty game folder has a sound");
        // the game's own file
        var own = In("romfs", "Sound", "Resource", "Stream", "SE_Sys_Do00.bwav");
        Put(own, sound);
        Check(Is(Mods.RomfsFile(game, Do), own) && Mods.RomfsFile(game, Do.ToUpperInvariant()) != null && Is(Mods.RomfsFile(game, Do.Replace('/', '\\')), own), "the game's own file");
        Check(Mods.RomfsFile(game, "Sound/Resource/Stream/SE_Sys_Cancel00.bwav") == null && Mods.RomfsFile(game, "") == null && Mods.RomfsFile(game, "../romfs/" + Do) == null && Mods.RomfsFile(game, "Sound/../" + Do) == null,
              "a file that is not there, or a path that is none");
        // a mod that carries it: the mod's, as the game would read it. Of two, the one that loads last
        var first = In("mods", "01-Chimes", "romfs", "Sound", "Resource", "Stream", "SE_Sys_Do00.bwav");
        Put(first, sound);
        Put(In("mods", "02-Grass", "romfs", "Pack", "grass.bin"), new byte[4]);
        Check(Is(Mods.RomfsFile(game, Do), first), "a mod's file in place of the game's");
        var second = In("mods", "03-Bells", "romfs", "sound", "resource", "STREAM", "se_sys_do00.BWAV");
        Put(second, sound);
        Check(Is(Mods.RomfsFile(game, Do), second), "of two mods' files, not the one of the mod that loads last");
        var deep = In("mods", "contents", Mods.TitleId, "Deep", "romfs", "Sound", "Resource", "Stream", "SE_Sys_Do00.bwav");
        Put(deep, sound);
        Check(Is(Mods.RomfsFile(game, Do), deep), "a mod in an emulator's layout, which sorts last here");
        // mods that are off do not count
        Directory.Move(In("mods", "contents"), In("mods", "-contents"));
        Check(Is(Mods.RomfsFile(game, Do), second), "a mod inside a folder that is off");
        Directory.Move(In("mods", "03-Bells"), In("mods", "-03-Bells"));
        Check(Is(Mods.RomfsFile(game, Do), first), "a mod that is off");
        Directory.Move(In("mods", "01-Chimes"), In("mods", "_01-Chimes"));
        Check(Is(Mods.RomfsFile(game, Do), own), "with every mod that carries it off, not the game's own again");
        // a folder of that name is no file; a mod without the game's own file is enough
        Directory.CreateDirectory(In("mods", "04-Odd", "romfs", "Sound", "Resource", "Stream", "SE_Sys_Do00.bwav"));
        Check(Is(Mods.RomfsFile(game, Do), own), "a folder was taken for the sound's file");
        File.Delete(own);
        Check(Mods.RomfsFile(game, Do) == null, "no file anywhere, and still one");
        Directory.Move(In("mods", "_01-Chimes"), In("mods", "01-Chimes"));
        Check(Is(Mods.RomfsFile(game, Do), first), "a mod's file where the game has none");
        // a mod whose romfs folder is a link: the game leaves it out
        var target = In("elsewhere");
        Put(Path.Combine(target, "Sound", "Resource", "Stream", "SE_Sys_Do00.bwav"), sound);
        Directory.CreateDirectory(In("mods", "09-Linked"));
        using (var p = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("cmd.exe", "/c mklink /J \"" + In("mods", "09-Linked", "romfs") + "\" \"" + target + "\"")
               { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true })!)
        {
            p.StandardOutput.ReadToEnd();
            p.WaitForExit();
        }
        if (File.Exists(In("mods", "09-Linked", "romfs", "Sound", "Resource", "Stream", "SE_Sys_Do00.bwav")))
        {
            Check(Mods.List(In("mods")).Any(m => m.Rel == "09-Linked") && Is(Mods.RomfsFile(game, Do), first), "a mod whose romfs folder is a link gave the sound");
            Directory.Delete(In("mods", "09-Linked", "romfs"));   // (the link, not what it points to)
        }
        else Console.WriteLine("NOT RUN: no link to a folder could be made here: a mod whose romfs is a link was not tried");
    }

    static void Playing(string game)
    {
        string Stream(string name) => Path.Combine(game, "romfs", "Sound", "Resource", "Stream", name);
        var decide = File_(48000, Pcm(0, 100, -200, 300), Pcm(1, 50, 60, 70));
        var cancel = File_(48000, Adpcm(2, new short[] { 0, 0 }, 4, (0, 2, new[] { 1, 2, -3, 4 })));
        var running = false;

        // a Setup that makes no sound (a test run, --snapshot): no file is asked for, nothing is played, whatever is asked of it
        Put(Stream("SE_Sys_Do00.bwav"), decide);
        var silent = new Sounds(null, () => throw new Exception("a Setup without sound asked whether the game runs"));
        using (new FileStream(Stream("SE_Sys_Do00.bwav"), FileMode.Open, FileAccess.Read, FileShare.None)) silent.Load(game);
        silent.Play(Sounds.Decide);
        silent.Level = "high";
        Check(silent.Level == "high", "a Setup without sound does not keep its setting");

        var player = new Noting();
        var sounds = new Sounds(player, () => running);
        // before a game was read, and for no game at all: silence
        sounds.Play(Sounds.Decide);
        sounds.Load(null);
        sounds.Play(Sounds.Decide);
        sounds.Load(Path.Combine(game, "not there"));
        sounds.Play(Sounds.Cancel);
        Check(player.Played.Count == 0, "a sound before any game was read");
        // the game's decide sound, at Low; no cancel sound in this game: silence for it
        sounds.Load(game);
        sounds.Play(Sounds.Decide);
        sounds.Play(Sounds.Cancel);
        sounds.Play("something else");
        Check(player.Played.Count == 1 && player.Played[0].What == "decide" && player.Played[0].Wav.Length == 44 + 3 * 4 && Peak(player.Played[0].Wav) == 5898, "the decide sound at Low");
        var low = player.Played[0].Wav;
        Check(BitConverter.ToInt16(low, 44 + 8) == 5898 && BitConverter.ToInt16(low, 44 + 10) == 1376 && BitConverter.ToInt16(low, 44 + 4) == -3932, "the decide sound's samples: left and right in turn");
        // the cancel sound comes: both play, each as itself
        Put(Stream("SE_Sys_Cancel00.bwav"), cancel);
        sounds.Load(game);
        sounds.Play(Sounds.Cancel);
        sounds.Play(Sounds.Decide);
        Check(player.Played.Count == 3 && player.Played[1].What == "cancel" && player.Played[1].Wav.Length == 44 + 4 * 4 && Peak(player.Played[1].Wav) == 5898 && player.Played[2].What == "decide", "the cancel sound");
        // nothing changed: nothing is read again (the player keeps the blocks it was handed, and gets the same ones)
        var forgot = player.Forgot;
        sounds.Load(game);
        sounds.Play(Sounds.Decide);
        Check(player.Forgot == forgot && ReferenceEquals(player.Played[3].Wav, player.Played[2].Wav), "sounds that did not change were made anew");
        // the setting: each level's share of full scale, Off silent, and what was handed out before is let go of
        foreach (var (level, peak) in new[] { ("medium", 13107), ("high", 24575), ("low", 5898) })
        {
            sounds.Level = level;
            sounds.Play(Sounds.Decide);
            Check(player.Forgot == ++forgot && Peak(player.Played.Last().Wav) == peak && sounds.Level == level, "the sound at " + level + ": " + Peak(player.Played.Last().Wav));
        }
        var played = player.Played.Count;
        sounds.Level = "off";
        sounds.Play(Sounds.Decide);
        sounds.Play(Sounds.Cancel);
        sounds.Load(game);
        sounds.Play(Sounds.Decide);
        Check(player.Played.Count == played && sounds.Level == "off", "a sound with the setting Off");
        sounds.Level = "nonsense";
        sounds.Play(Sounds.Decide);
        Check(player.Played.Count == ++played && Peak(player.Played.Last().Wav) == 5898 && sounds.Level == "low", "a setting that is no choice is not Low");
        // while the game runs: silence; after it: sound again
        running = true;
        sounds.Play(Sounds.Decide);
        sounds.Play(Sounds.Cancel);
        Check(player.Played.Count == played, "a sound while the game runs");
        running = false;
        sounds.Play(Sounds.Cancel);
        Check(player.Played.Count == ++played, "no sound after the game has ended");

        // a mod that replaces the decide sound: the mod's plays, as in the game; switched off, the game's own again
        var mod = Path.Combine(game, "mods", "Chimes", "romfs", "Sound", "Resource", "Stream", "SE_Sys_Do00.bwav");
        Put(mod, File_(48000, Pcm(2, 9, 8, 7, 6, 5, 4, 3)));
        forgot = player.Forgot;
        sounds.Load(game);
        sounds.Play(Sounds.Decide);
        Check(player.Played.Last().Wav.Length == 44 + 7 * 4 && player.Forgot == forgot + 1, "a mod's decide sound in place of the game's");
        Directory.Move(Path.Combine(game, "mods", "Chimes"), Path.Combine(game, "mods", "-Chimes"));
        sounds.Load(game);
        sounds.Play(Sounds.Decide);
        Check(player.Played.Last().Wav.Length == 44 + 3 * 4, "with the mod off, not the game's own sound again");
        // the same file, written anew with other samples: taken up
        Put(Stream("SE_Sys_Do00.bwav"), File_(48000, Pcm(2, 1, 2, 3, 4, 5)));
        sounds.Load(game);
        sounds.Play(Sounds.Decide);
        Check(player.Played.Last().Wav.Length == 44 + 5 * 4, "a sound file that changed was not read again");

        // files that do not decode, are cut off, are silence or are far too large: silence, and nothing thrown
        played = player.Played.Count;
        foreach (var bad in new[] { Encoding.ASCII.GetBytes("RIFF, a WAV file under the name"), decide.Take(decide.Length - 3).ToArray(), new byte[0], File_(48000, Pcm(2, 0, 0, 0, 0)),
                                    decide.Concat(new byte[Sounds.MaxFile]).ToArray() })
        {
            Put(Stream("SE_Sys_Do00.bwav"), bad);
            sounds.Load(game);
            sounds.Play(Sounds.Decide);
            Check(player.Played.Count == played, "a file of " + bad.Length + " bytes that is no sound was played");
            sounds.Play(Sounds.Cancel);
            Check(player.Played.Count == ++played && player.Played.Last().What == "cancel", "the other sound went with the one that does not decode");
        }
        // (a file that cannot be opened: silence too)
        Put(Stream("SE_Sys_Do00.bwav"), decide);
        using (new FileStream(Stream("SE_Sys_Do00.bwav"), FileMode.Open, FileAccess.Read, FileShare.None)) sounds.Load(game);
        sounds.Play(Sounds.Decide);
        sounds.Load(game);

        // a PC that cannot play (no sound device): nothing is thrown, and it is not tried again and again
        var none = new Noting { Fails = true };
        var mute = new Sounds(none, () => false);
        mute.Load(game);
        mute.Play(Sounds.Cancel);
        none.Fails = false;
        mute.Play(Sounds.Cancel);
        Check(none.Played.Count == 0, "a player that failed was asked again");

        // (testing) the sounds written down instead of played
        var log = Path.Combine(game, "sounds.txt");
        var noted = new Sounds(new NotedPlayer(log), () => false, "high");
        noted.Load(game);
        noted.Play(Sounds.Cancel);
        noted.Level = "off";
        noted.Play(Sounds.Cancel);
        Check(File.ReadAllLines(log).SequenceEqual(new[] { "cancel 60 bytes, peak 24575" }), "the sounds written down: " + File.ReadAllText(log));
    }
}
