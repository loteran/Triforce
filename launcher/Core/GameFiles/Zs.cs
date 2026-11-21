// The game's .zs files: zstd frames made with one of three dictionaries that the game keeps in its romfs, in
// "Pack/ZsDic.pack.zs" (a zstd frame without a dictionary holding a SARC archive of the dictionaries). Ported from the
// lab's research/scratch/t09_common.py (unzs, sarc).
using System.Buffers.Binary;
using System.Text;
using ZstdSharp;

namespace Triforce.Core.GameFiles;

/// <summary>Unpacks romfs .zs files with the game's own dictionaries. Thread-safe.</summary>
public sealed class Zs
{
    public const string DictionaryPack = "Pack/ZsDic.pack.zs";

    readonly IGameSource source;
    readonly Lazy<Dictionary<string, byte[]>> dictionaries;

    public Zs(IGameSource source)
    {
        this.source = source;
        dictionaries = new Lazy<Dictionary<string, byte[]>>(() => Sarc(Decompress(source.Read(DictionaryPack), null)));
    }

    /// <summary>The game's dictionaries by name (zs.zsdic, pack.zsdic, bcett.byml.zsdic).</summary>
    public IReadOnlyDictionary<string, byte[]> Dictionaries => dictionaries.Value;

    /// <summary>The dictionary the game uses for a file, by its name (t09_common.unzs): bcett files have their own, .pack.zs
    /// archives theirs, everything else the general one.</summary>
    public static string DictionaryFor(string romfsPath)
    {
        var name = romfsPath[(romfsPath.LastIndexOf('/') + 1)..];
        return name.Contains("bcett", StringComparison.Ordinal) ? "bcett.byml.zsdic"
            : name.EndsWith(".pack.zs", StringComparison.Ordinal) ? "pack.zsdic" : "zs.zsdic";
    }

    /// <summary>A romfs .zs file, read and unpacked.</summary>
    public byte[] Unpack(string romfsPath) => Unpack(romfsPath, source.Read(romfsPath));

    /// <summary>A .zs file's bytes unpacked. The dictionary is the one the name calls for; when the frame names another
    /// dictionary by its id, that one is used instead.</summary>
    public byte[] Unpack(string romfsPath, byte[] data)
    {
        var dicts = dictionaries.Value;
        dicts.TryGetValue(DictionaryFor(romfsPath), out var dict);
        var wanted = FrameDictionaryId(data);
        if (wanted != 0 && (dict == null || DictionaryId(dict) != wanted))
            dict = dicts.Values.FirstOrDefault(d => DictionaryId(d) == wanted) ?? dict;
        return Decompress(data, dict);
    }

    /// <summary>One zstd frame unpacked, with a dictionary or none.</summary>
    public static byte[] Decompress(ReadOnlySpan<byte> data, byte[]? dictionary)
    {
        using var z = new Decompressor();
        if (dictionary != null)
            z.LoadDictionary(dictionary);
        var size = Decompressor.GetDecompressedSize(data);
        if (size > 0 && size <= int.MaxValue)
        {
            var output = new byte[size];
            var n = z.Unwrap(data, output);
            return n == output.Length ? output : output[..n];
        }
        // (a frame that does not say how large it is: unpacked as a stream)
        using var stream = new DecompressionStream(new MemoryStream(data.ToArray()));
        if (dictionary != null)
            stream.LoadDictionary(dictionary);
        using var into = new MemoryStream();
        stream.CopyTo(into);
        return into.ToArray();
    }

    /// <summary>The dictionary id a zstd frame header names, 0 for none.</summary>
    static uint FrameDictionaryId(ReadOnlySpan<byte> frame)
    {
        if (frame.Length < 6 || BinaryPrimitives.ReadUInt32LittleEndian(frame) != 0xFD2FB528)
            return 0;
        var fhd = frame[4];
        var single = (fhd >> 5) & 1;
        var idBytes = new[] { 0, 1, 2, 4 }[fhd & 3];
        var at = 5 + (single != 0 ? 0 : 1);
        uint id = 0;
        for (var k = idBytes - 1; k >= 0; k--)
            id = id << 8 | (at + k < frame.Length ? frame[at + k] : 0u);
        return id;
    }

    /// <summary>A zstd dictionary's id (after its magic 0xEC30A437), 0 for raw content.</summary>
    static uint DictionaryId(byte[] dict) =>
        dict.Length >= 8 && BinaryPrimitives.ReadUInt32LittleEndian(dict) == 0xEC30A437 ? BinaryPrimitives.ReadUInt32LittleEndian(dict.AsSpan(4)) : 0;

    /// <summary>A SARC archive's files by name (by hash, as 8 hex digits, for one without a name). Little-endian SARC only,
    /// as this game's are.</summary>
    public static Dictionary<string, byte[]> Sarc(byte[] b)
    {
        if (b.Length < 0x20 || b[0] != 'S' || b[1] != 'A' || b[2] != 'R' || b[3] != 'C')
            throw new InvalidDataException("not a SARC archive");
        var dataAt = BinaryPrimitives.ReadInt32LittleEndian(b.AsSpan(0x0C));
        const int sfat = 0x14;
        var count = BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan(sfat + 6));
        var namesAt = sfat + 12 + 16 * count + 8;
        var files = new Dictionary<string, byte[]>(StringComparer.Ordinal);
        for (var i = 0; i < count; i++)
        {
            var node = b.AsSpan(sfat + 12 + 16 * i);
            var hash = BinaryPrimitives.ReadUInt32LittleEndian(node);
            var attr = BinaryPrimitives.ReadUInt32LittleEndian(node[4..]);
            var start = BinaryPrimitives.ReadInt32LittleEndian(node[8..]);
            var end = BinaryPrimitives.ReadInt32LittleEndian(node[12..]);
            string name;
            if (attr >> 24 != 0)
            {
                var at = namesAt + (int)(attr & 0xFFFF) * 4;
                var zero = Array.IndexOf(b, (byte)0, at);
                name = Encoding.UTF8.GetString(b, at, (zero < 0 ? b.Length : zero) - at);
            }
            else
                name = hash.ToString("x8");
            files[name] = b[(dataAt + start)..(dataAt + end)];
        }
        return files;
    }
}
