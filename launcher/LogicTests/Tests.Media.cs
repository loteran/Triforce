using System.Text;
using Triforce.Core.GameFiles;
using static Triforce.LogicTests.T;

namespace Triforce.LogicTests;

static partial class Tests
{
    // ---- the launcher's music: the game's loop from the BWAV through the mix into the WAV the page plays ----
    // (every sound here is made by the test: no game file is read)

    /// <summary>A BWAV file laid out as the game's are, with PCM16 channels (one in the middle, or two: left and right) and
    /// a loop from loopStart to just before loopEnd (0xFFFFFFFF: none, as the item fanfare and the interface sounds).</summary>
    static byte[] TestBwav(short[][] channels, uint loopStart, uint loopEnd, int rate = 48000)
    {
        var frames = channels[0].Length;
        var at = (0x10 + channels.Length * 0x4C + 0x3F) & ~0x3F;   // (the samples start at 0xC0 in the game's two-channel files)
        var b = new byte[at + channels.Length * frames * 2];
        void Put(int o, long v, int n) { for (var i = 0; i < n; i++) b[o + i] = (byte)(v >> 8 * i); }
        Encoding.ASCII.GetBytes("BWAV").CopyTo(b, 0);
        Put(4, 0xFEFF, 2); Put(6, 1, 2); Put(0x0E, channels.Length, 2);
        for (var c = 0; c < channels.Length; c++)
        {
            int info = 0x10 + c * 0x4C, data = at + c * frames * 2;
            Put(info, 0, 2); Put(info + 2, channels.Length == 1 ? 2 : c, 2); Put(info + 4, rate, 4); Put(info + 8, frames, 4); Put(info + 0xC, frames, 4);
            Put(info + 0x30, data, 4); Put(info + 0x34, data, 4); Put(info + 0x38, 1, 4); Put(info + 0x3C, loopEnd, 4); Put(info + 0x40, loopStart, 4);
            for (var i = 0; i < frames; i++)
                Put(data + 2 * i, channels[c][i], 2);
        }
        return b;
    }

    static short[] TestTone(int frames, int step) => Enumerable.Range(0, frames).Select(i => (short)(i * step % 2000 - 1000)).ToArray();

    static void MusicLoop()
    {
        // (1000 frames: an intro of 300, then the loop to the end, as the game's songs are laid out)
        short[] left = TestTone(1000, 7), right = TestTone(1000, 11);
        (int Start, int End)? want = (300, 1000);
        var pcm = Bwav.Decode(TestBwav(new[] { left, right }, 300, 1000));
        Equal(1000, pcm.Frames, "frames");
        Equal(want, pcm.Loop, "the header's loop: its start, and its end the sample after its last");
        Check(Bwav.Decode(TestBwav(new[] { left, right }, 0, 0xFFFFFFFF)).Loop == null, "end 0xFFFFFFFF: no loop");
        Check(Bwav.Decode(TestBwav(new[] { left, right }, 1000, 1000)).Loop == null, "a loop that starts at its end is none");
        Equal(want, Bwav.Decode(TestBwav(new[] { left, right }, 300, 5000)).Loop, "an end past the sound is cut to it");
        Equal(want, Bwav.Decode(TestBwav(new[] { left }, 300, 1000)).Loop, "one channel");

        // the mix (the eight layers of Saria's Song share one loop) keeps it; layers with other loops have none
        var layer = Bwav.Decode(TestBwav(new[] { right, left }, 300, 1000));
        var mixed = Bwav.Mix(pcm, layer);
        Equal(want, mixed.Loop, "a mix of layers with the same loop");
        Check(Bwav.Mix(pcm, Bwav.Decode(TestBwav(new[] { right, left }, 200, 1000))).Loop == null, "a mix of layers with other loops: none");
        Check(Bwav.Mix(pcm, Bwav.Decode(TestBwav(new[] { right, left }, 0, 0xFFFFFFFF))).Loop == null, "a mix with a layer that does not loop: none");
        Equal(want, Bwav.Normalize(pcm).Loop, "normalized");
        Equal(want, Bwav.Gain(pcm, .5).Loop, "made quieter");

        // the WAV: the 44-byte header and the samples at 44 as before, then a smpl chunk with the loop, its last sample in
        var wav = Bwav.ToWav(mixed);
        var data = mixed.Samples.Length * 2;
        Equal(44 + data + Bwav.SmplChunkSize, wav.Length, "length with the smpl chunk");
        Equal("RIFF", Encoding.ASCII.GetString(wav, 0, 4), "RIFF");
        Equal(wav.Length - 8, BitConverter.ToInt32(wav, 4), "the RIFF size takes the smpl chunk in");
        Equal("data", Encoding.ASCII.GetString(wav, 36, 4), "the data chunk where it was");
        Equal(data, BitConverter.ToInt32(wav, 40), "data size");
        Check(Bwav.FromWav(wav).Samples.SequenceEqual(mixed.Samples), "the samples read back");
        var smpl = 44 + data;
        Equal("smpl", Encoding.ASCII.GetString(wav, smpl, 4), "smpl after the data");
        Equal(60, BitConverter.ToInt32(wav, smpl + 4), "smpl size: 36 and one loop");
        Equal(1_000_000_000 / 48000, BitConverter.ToInt32(wav, smpl + 8 + 8), "the sample period in ns");
        Equal(60, BitConverter.ToInt32(wav, smpl + 8 + 12), "MIDI unity note");
        Equal(1, BitConverter.ToInt32(wav, smpl + 8 + 28), "one loop");
        Equal(0, BitConverter.ToInt32(wav, smpl + 8 + 40), "forward");
        Equal(300, BitConverter.ToInt32(wav, smpl + 8 + 44), "loop start");
        Equal(999, BitConverter.ToInt32(wav, smpl + 8 + 48), "the loop's last sample");
        Equal(0, BitConverter.ToInt32(wav, smpl + 8 + 56), "play count 0: for ever");
        Equal(want, Bwav.FromWav(wav).Loop, "the loop read back");

        // a sound that plays once: the WAV as before (44 bytes and the samples), no smpl
        var once = Bwav.Decode(TestBwav(new[] { left, right }, 0, 0xFFFFFFFF));
        var plain = Bwav.ToWav(once);
        Equal(44 + once.Samples.Length * 2, plain.Length, "no loop, no smpl chunk");
        Equal(36 + once.Samples.Length * 2, BitConverter.ToInt32(plain, 4), "its RIFF size as before");
        Check(Bwav.FromWav(plain).Loop == null, "read back without a loop");

        // a smpl loop that is not inside the sound, or a smpl without loops, reads as no loop
        var past = (byte[])wav.Clone();
        BitConverter.TryWriteBytes(past.AsSpan(smpl + 8 + 48), 1000);
        Check(Bwav.FromWav(past).Loop == null, "a loop whose last sample is past the sound");
        var backwards = (byte[])wav.Clone();
        BitConverter.TryWriteBytes(backwards.AsSpan(smpl + 8 + 44), 1000);
        Check(Bwav.FromWav(backwards).Loop == null, "a loop that ends before it starts");
        var none = (byte[])wav.Clone();
        BitConverter.TryWriteBytes(none.AsSpan(smpl + 8 + 28), 0);
        Check(Bwav.FromWav(none).Loop == null, "a smpl chunk with no loop");
        Check(Bwav.FromWav(wav.AsSpan(0, smpl + 8 + 40).ToArray()).Loop == null, "a smpl chunk cut off");
    }

    static void MusicCache()
    {
        var root = NewRoot("media-loop");
        var stream = Path.Combine(root, "romfs", "Sound", "Resource", "Stream");
        Directory.CreateDirectory(stream);
        short[] tone = TestTone(1000, 7);
        File.WriteAllBytes(Path.Combine(stream, "BGM_GreatFairyFountain.bwav"), TestBwav(new[] { tone, tone }, 300, 1000));
        File.WriteAllBytes(Path.Combine(stream, "BGM_ItemGet_Large.bwav"), TestBwav(new[] { tone, tone }, 0, 0xFFFFFFFF));
        var cache = Path.Combine(root, "media");
        Directory.CreateDirectory(cache);
        // (what an older launcher left: the same name with another key, and no loop in it)
        var old = Path.Combine(cache, "fairy-0123456789abcdef.wav");
        File.WriteAllBytes(old, Bwav.ToWav(new Pcm { Rate = 48000, Samples = new short[20] }));

        using var source = new FolderSource(root);
        var media = new GameMedia(source, cache);
        var file = media.WavFile("fairy");
        Check(!File.Exists(old), "the older launcher's WAV is replaced");
        Check(Path.GetFileName(file).StartsWith("fairy-", StringComparison.Ordinal) && Path.GetFileName(file).Length == Path.GetFileName(old).Length, "named as before: " + file);
        (int Start, int End)? want = (300, 1000);
        Equal(want, Bwav.FromWav(File.ReadAllBytes(file)).Loop, "the cached WAV has the game's loop");
        Equal(file, new GameMedia(source, cache).WavFile("fairy"), "read back from the cache the next time");
        Check(Bwav.FromWav(media.Wav("itemget")).Loop == null, "the fanfare plays once");
    }
}
