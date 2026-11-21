// BWAV: the game's sound files (romfs "Sound/Resource/Stream/<name>.bwav"). Ported from setup/Sound.cs (Bwav.Decode,
// from NX2navi's runtime/menu_sound.c) and the lab's research/scratch/bwav2wav.py and mixwav.py, which made the sounds the
// launcher's design was approved with; this decodes and mixes the same way, sample for sample.
//
// Layout: "BWAV", a byte order mark, a version, a CRC32, a prefetch flag (u16) and the channel count (u16 at 0x0E), then
// 0x4C bytes per channel: codec (u16: 0 PCM16, 1 DSP-ADPCM), pan (u16: 0 left, 1 right, 2 middle), sample rate (u32),
// sample count (u32), the count again, 16 DSP coefficients (s16), the place of the samples in the file (u32 at 0x30) and
// again (0x34), a u32 that is 1 in every file, looped or not (0x38), the loop's end (u32 at 0x3C: the sample after its
// last one, 0xFFFFFFFF for a sound that does not loop) and its start (u32 at 0x40), then the ADPCM state at the loop.
// The music loops that way: Saria's Song, the Fairy Fountain and the Temple of Time play an intro once and then loop to
// their end from a sample after it; the item fanfare and the interface sounds have no loop.
// DSP-ADPCM is frames of 8 bytes: a header (the coefficient pair's index, the scale's exponent) and 14 samples of four bits.
// All numbers little-endian. Other codecs (the title music's codec 2) are not decoded.
namespace Triforce.Core.GameFiles;

/// <summary>16-bit stereo sound: left and right samples in turn.</summary>
public sealed class Pcm
{
    public int Rate { get; init; }
    public short[] Samples { get; init; } = Array.Empty<short>();
    public int Frames => Samples.Length / 2;
    public double Seconds => Rate > 0 ? (double)Frames / Rate : 0;

    /// <summary>The game's loop, in frames: from Start to just before End, which is at most Frames. Null for a sound that
    /// plays once (or a WAV file without a loop).</summary>
    public (int Start, int End)? Loop { get; init; }

    /// <summary>The loudest sample's size (0 for silence).</summary>
    public int Peak
    {
        get
        {
            var peak = 0;
            foreach (var s in Samples)
                peak = Math.Max(peak, Math.Abs((int)s));
            return peak;
        }
    }
}

public static class Bwav
{
    /// <summary>The share of full scale the loudest sample is set to by Mix and Normalize (as mixwav.py: 70 %).</summary>
    public const double DefaultPeak = 0.7;

    static int U16(byte[] b, long at) => b[at] | b[at + 1] << 8;
    static long U32(byte[] b, long at) => b[at] | (long)b[at + 1] << 8 | (long)b[at + 2] << 16 | (long)b[at + 3] << 24;

    /// <summary>
    /// A BWAV file decoded to 16-bit stereo, as bwav2wav.py does at gain 1: each channel goes to its side by its pan (left,
    /// right, or both), a one-channel sound is lowered by 0.7071 to sound as loud in both, and each sample is cut toward
    /// zero to a whole number and clamped. Throws InvalidDataException for anything that is not a whole BWAV file of one or
    /// two channels of PCM16 or DSP-ADPCM.
    /// </summary>
    public static Pcm Decode(byte[] b)
    {
        if (b.Length < 0x10 || b[0] != 'B' || b[1] != 'W' || b[2] != 'A' || b[3] != 'V')
            throw new InvalidDataException("not a BWAV file");
        var channels = U16(b, 0x0E);
        if (channels < 1 || channels > 2 || 0x10 + channels * 0x4C > b.Length)
            throw new InvalidDataException("BWAV with " + channels + " channels (one or two are decoded)");
        long rate = U32(b, 0x10 + 4), frames = U32(b, 0x10 + 8);
        if (rate < 8000 || rate > 96000 || frames == 0 || frames > rate * 3600)
            throw new InvalidDataException("BWAV with a rate of " + rate + " and " + frames + " samples");
        for (var c = 0; c < channels; c++)
            if (!Whole(b, 0x10 + c * 0x4C, frames))
                throw new InvalidDataException("BWAV channel " + c + " is cut off or of an unknown codec (" + U16(b, 0x10 + c * 0x4C) + ")");

        // (the sides summed as whole numbers, as bwav2wav.py does: two channels panned to the same side add up)
        var left = new int[frames];
        var right = new int[frames];
        for (var c = 0; c < channels; c++)
            Channel(b, 0x10 + c * 0x4C, left, right, frames);

        var samples = new short[frames * 2];
        for (long i = 0; i < frames; i++)
        {
            double l = left[i], r = right[i];
            if (channels == 1)
            {
                l *= 0.7071;
                r *= 0.7071;
            }
            samples[2 * i] = (short)Math.Clamp((int)l, -32768, 32767);
            samples[2 * i + 1] = (short)Math.Clamp((int)r, -32768, 32767);
        }
        return new Pcm { Rate = (int)rate, Samples = samples, Loop = LoopOf(b, 0x10, frames) };
    }

    /// <summary>The loop a channel's header gives (its end at 0x3C, its start at 0x40), cut to the sound's length; null for
    /// a sound that does not loop (end 0xFFFFFFFF) or a loop that is none (its start not before its end). The first
    /// channel's: the game's files give every channel the same.</summary>
    static (int Start, int End)? LoopOf(byte[] b, long info, long frames)
    {
        long end = U32(b, info + 0x3C), start = U32(b, info + 0x40);
        if (end == 0xFFFFFFFF)
            return null;
        end = Math.Min(end, frames);
        if (start >= end)
            return null;
        return ((int)start, (int)end);
    }

    /// <summary>Is the channel one of the two codecs, with all of its samples (as many as the sound has, at most) inside the
    /// file?</summary>
    static bool Whole(byte[] b, long info, long frames)
    {
        var codec = U16(b, info);
        long count = Math.Min(U32(b, info + 8), frames), at = U32(b, info + 0x30);
        // (DSP-ADPCM: the last frame may be cut to the bytes its samples use, its header and one byte per two samples; the
        // game's music files end that way, so asking for the whole 8 bytes would refuse them)
        var rest = count % 14;
        var bytes = count / 14 * 8 + (rest == 0 ? 0 : 1 + (rest + 1) / 2);
        return codec == 0 ? at + count * 2 <= b.Length : codec == 1 && at + bytes <= b.Length;
    }

    /// <summary>One channel added into its side: pan 0 left, 1 right, anything else both.</summary>
    static void Channel(byte[] b, long info, int[] left, int[] right, long frames)
    {
        int codec = U16(b, info), pan = U16(b, info + 2);
        long count = Math.Min(U32(b, info + 8), frames), at = U32(b, info + 0x30);
        if (codec == 0)
        {
            for (long i = 0; i < count; i++)
            {
                int v = (short)U16(b, at + 2 * i);
                if (pan != 1) left[i] += v;
                if (pan != 0) right[i] += v;
            }
            return;
        }
        var pairs = new int[16];
        for (var i = 0; i < 16; i++)
            pairs[i] = (short)U16(b, info + 0x10 + 2 * i);
        long h1 = 0, h2 = 0;
        for (long i = 0; i < count; i++)
        {
            var frame = at + i / 14 * 8;
            var k = (int)(i % 14);
            int index = b[frame] >> 4 & 7, shift = b[frame] & 15;
            var nibble = b[frame + 1 + k / 2] >> ((k & 1) != 0 ? 0 : 4) & 15;
            if (nibble >= 8)
                nibble -= 16;
            var s = ((long)nibble << shift << 11) + pairs[2 * index] * h1 + pairs[2 * index + 1] * h2;
            var v = (int)Math.Max(-32768, Math.Min(32767, (s + 1024) >> 11));
            h2 = h1;
            h1 = v;
            if (pan != 1) left[i] += v;
            if (pan != 0) right[i] += v;
        }
    }

    /// <summary>
    /// Tracks of the same rate summed sample by sample (a shorter one is silence after its end), then scaled so the loudest
    /// sample is at `peak` of full scale, each sample cut toward zero: mixwav.py. One track in gives that track normalized.
    /// Saria's Song is the eight BGM_LostWood_0 .. _7 layers mixed this way. The mix keeps the tracks' loop when they all
    /// have the same one (the eight layers do), else it has none.
    /// </summary>
    public static Pcm Mix(double peak, params Pcm[] tracks)
    {
        if (tracks.Length == 0)
            throw new ArgumentException("nothing to mix");
        var length = tracks.Max(t => t.Samples.Length);
        var rate = tracks[^1].Rate;
        var sum = new int[length];
        foreach (var t in tracks)
        {
            if (t.Rate != rate)
                throw new ArgumentException("tracks of different rates (" + t.Rate + ", " + rate + ") cannot be mixed");
            var s = t.Samples;
            for (var i = 0; i < s.Length; i++)
                sum[i] += s[i];
        }
        var loudest = 1;
        foreach (var v in sum)
            loudest = Math.Max(loudest, Math.Abs(v));
        var gain = peak * 32767 / loudest;
        var output = new short[length];
        for (var i = 0; i < length; i++)
            output[i] = (short)Math.Clamp((int)(sum[i] * gain), -32768, 32767);
        var loop = tracks[0].Loop;
        if (tracks.Any(t => t.Loop != loop))
            loop = null;
        return new Pcm { Rate = rate, Samples = output, Loop = loop };
    }

    /// <summary>Mix at the default peak (70 % of full scale).</summary>
    public static Pcm Mix(params Pcm[] tracks) => Mix(DefaultPeak, tracks);

    /// <summary>One track with its loudest sample at `peak` of full scale (Mix of one track).</summary>
    public static Pcm Normalize(Pcm track, double peak = DefaultPeak) => Mix(peak, track);

    /// <summary>One track made louder or quieter by a factor, clamped at full scale.</summary>
    public static Pcm Gain(Pcm track, double gain)
    {
        var s = track.Samples;
        var output = new short[s.Length];
        for (var i = 0; i < s.Length; i++)
            output[i] = (short)Math.Clamp((int)(s[i] * gain), -32768, 32767);
        return new Pcm { Rate = track.Rate, Samples = output, Loop = track.Loop };
    }

    /// <summary>The size of the smpl chunk ToWav writes for a loop: its 8-byte head, 36 bytes and one loop of 24.</summary>
    public const int SmplChunkSize = 8 + 36 + 24;

    /// <summary>
    /// The sound as a WAV file's bytes: 16-bit stereo PCM with the usual 44-byte header. A sound with a loop gets a smpl
    /// chunk after its data (the samples stay at byte 44, as in a file without one): one forward loop that plays for ever,
    /// from the loop's start to its last sample (smpl counts the end in), which is what the launcher's page loops between.
    /// </summary>
    public static byte[] ToWav(Pcm pcm)
    {
        var data = pcm.Samples.Length * 2;
        var smpl = pcm.Loop.HasValue ? SmplChunkSize : 0;
        var wav = new byte[44 + data + smpl];
        void Tag(int at, string tag) { for (var i = 0; i < 4; i++) wav[at + i] = (byte)tag[i]; }
        void Put(int at, int value, int bytes) { for (var i = 0; i < bytes; i++) wav[at + i] = (byte)(value >> 8 * i); }
        Tag(0, "RIFF"); Put(4, 36 + data + smpl, 4); Tag(8, "WAVE");
        Tag(12, "fmt "); Put(16, 16, 4); Put(20, 1, 2); Put(22, 2, 2); Put(24, pcm.Rate, 4); Put(28, pcm.Rate * 4, 4); Put(32, 4, 2); Put(34, 16, 2);
        Tag(36, "data"); Put(40, data, 4);
        System.Runtime.InteropServices.MemoryMarshal.AsBytes(pcm.Samples.AsSpan()).CopyTo(wav.AsSpan(44));
        if (!BitConverter.IsLittleEndian)
            for (var i = 44; i + 1 < 44 + data; i += 2)
                (wav[i], wav[i + 1]) = (wav[i + 1], wav[i]);
        if (pcm.Loop is { } loop)
        {
            // (manufacturer, product, the sample period in ns, MIDI note 60, no pitch fraction, no SMPTE, one loop, no
            // sampler data; then the loop: cue 0, type 0 = forward, start, last sample, no fraction, play count 0 = for ever)
            var at = 44 + data;
            Tag(at, "smpl"); Put(at + 4, 36 + 24, 4);
            Put(at + 16, pcm.Rate > 0 ? 1_000_000_000 / pcm.Rate : 0, 4); Put(at + 20, 60, 4); Put(at + 36, 1, 4);
            Put(at + 52, loop.Start, 4); Put(at + 56, loop.End - 1, 4);
        }
        return wav;
    }

    /// <summary>The first loop of a WAV file's smpl chunk (its body at `at`, `n` bytes), with its end made the sample after
    /// the last (null without a loop, or with a loop that ends before it starts).</summary>
    static (int Start, int End)? SmplLoop(byte[] wav, int at, int n)
    {
        if (n < 36 + 24 || at + 36 + 24 > wav.Length || U32(wav, at + 28) == 0)
            return null;
        long start = U32(wav, at + 36 + 8), last = U32(wav, at + 36 + 12);
        if (start > last || last >= int.MaxValue)
            return null;
        return ((int)start, (int)last + 1);
    }

    /// <summary>A 16-bit stereo WAV file read back (for comparing with references; the chunks are found by their tags), with
    /// the loop of its smpl chunk when it has one inside the sound.</summary>
    public static Pcm FromWav(byte[] wav)
    {
        if (wav.Length < 12 || wav[0] != 'R' || wav[8] != 'W')
            throw new InvalidDataException("not a WAV file");
        int rate = 0, at = 12;
        short[]? samples = null;
        (int Start, int End)? loop = null;
        while (at + 8 <= wav.Length)
        {
            var tag = System.Text.Encoding.ASCII.GetString(wav, at, 4);
            var n = (int)Math.Min(U32(wav, at + 4), wav.Length - at - 8);
            if (tag == "fmt ")
            {
                if (U16(wav, at + 8 + 2) != 2 || U16(wav, at + 8 + 14) != 16)
                    throw new NotSupportedException("only 16-bit stereo WAV files are read");
                rate = (int)U32(wav, at + 8 + 4);
            }
            else if (tag == "data" && samples == null)
            {
                samples = new short[n / 2];
                System.Runtime.InteropServices.MemoryMarshal.Cast<byte, short>(wav.AsSpan(at + 8, n / 2 * 2)).CopyTo(samples);
            }
            else if (tag == "smpl")
                loop = SmplLoop(wav, at + 8, n);
            at += 8 + n + (n & 1);
        }
        if (samples == null)
            throw new InvalidDataException("WAV without data");
        if (loop is { } l && l.End > samples.Length / 2)
            loop = null;
        return new Pcm { Rate = rate, Samples = samples, Loop = loop };
    }
}
