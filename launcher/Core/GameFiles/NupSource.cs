// A .nup package: a decrypted Switch 2 dump in one file, holding one or more titles (the game, its update), each with an
// exefs/ and a romfs/ tree. Ported from nx2/nup.py (the reader the build uses); the layout, as worked out there
// (little-endian):
//
//   0x000  "NUP\x01"; u64 TOC offset at +0x08, u64 TOC size at +0x10, u64 blob offset at +0x38, u64 file size at +0x40
//   0x080  "TOC\x01", u32 titles, u64 dirs, u64 files, u64 strings offset (from 0x80), u64 strings size
//   0x0b0  titles, 0x40 each: u64 title id, u32 root dir, u32 exefs dir, u32 romfs dir, u32 version, ...
//   then   dirs, 24 bytes each: u32 name offset, u32 name length, u32 first child, u32 children, u64 0
//   then   files, 80 bytes each: u32 name offset, u32 name length, u64 size, u64 data offset, u64 stored size,
//          32-byte hash, u8 codec (0 raw, 1 zstd), u8 flags, u16 reserved, 12 bytes reserved
//   then   the child list, u32 each: a node index; below the dir count a dir, else (index - dirs) a file
//   then   the name strings (not 0-terminated)
//
// A file's data is stored as is at its data offset, or as a zstd frame whose content is the file (codec 1; in older
// packages codec 0 with a frame whose stated content size is the file's size). A romfs .zs file is zstd in the game too:
// that one is not unpacked here (Zs does it, with the game's dictionaries).
//
// The package is opened for reading only (FileAccess.Read, FileShare.Read) and never written. Opening reads the header and
// maps the index; no file data is read until a file is asked for.
using System.IO.MemoryMappedFiles;
using System.Text;
using Microsoft.Win32.SafeHandles;
using ZstdSharp;

namespace Triforce.Core.GameFiles;

/// <summary>One title in a .nup package: the base game or an update.</summary>
public readonly record struct NupTitle(int Index, ulong Id, uint Version)
{
    /// <summary>The title id as the Switch writes it (16 hex digits).</summary>
    public string IdHex => Id.ToString("x16");
    public override string ToString() => IdHex + " v" + Version;
}

/// <summary>Reads romfs files out of a .nup package. The newest title (the update) is looked in first, then the others.</summary>
public sealed class NupSource : IGameSource
{
    static readonly byte[] ZstdMagic = { 0x28, 0xB5, 0x2F, 0xFD };
    const uint None = 0xFFFFFFFF;

    readonly FileStream file;
    readonly SafeFileHandle handle;
    readonly MemoryMappedFile map;
    readonly MemoryMappedViewAccessor index;
    readonly long length;
    readonly DateTime written;
    readonly uint dirCount, fileCount;
    readonly long dirsAt, filesAt, kidsAt, stringsAt;
    readonly (uint Root, uint Exefs, uint Romfs)[] trees;
    // (each folder's children by name, made the first time the folder is looked into)
    readonly Dictionary<uint, Dictionary<string, uint>> children = new();

    /// <summary>The package's file.</summary>
    public string Path { get; }

    /// <summary>The titles in the package, newest version first (the update before the base game).</summary>
    public IReadOnlyList<NupTitle> Titles { get; }

    /// <summary>The newest title's version: the game version the player has (for this game, 131072 is 2.0.0).</summary>
    public uint Version => Titles[0].Version;

    public NupSource(string path)
    {
        Path = System.IO.Path.GetFullPath(path);
        file = new FileStream(Path, FileMode.Open, FileAccess.Read, FileShare.Read, 1, FileOptions.RandomAccess);
        try
        {
            handle = file.SafeFileHandle;
            var info = new FileInfo(Path);
            length = file.Length;
            written = info.LastWriteTimeUtc;

            var head = new byte[0xB0];
            if (RandomAccess.Read(handle, head, 0) != head.Length || !head.AsSpan(0, 4).SequenceEqual("NUP\x01"u8))
                throw new InvalidDataException(Path + " is not a .nup package");
            var declared = (long)BitConverter.ToUInt64(head, 0x40);
            // (a package that ends early, a download or copy that did not finish: said now, with the sizes)
            if (declared != 0 && length < declared)
                throw new InvalidDataException(string.Format("The .nup file is incomplete: it is {0} MB, its header says {1} MB. " +
                    "Copy or download it again.", length >> 20, declared >> 20));
            if (!head.AsSpan(0x80, 4).SequenceEqual("TOC\x01"u8))
                throw new InvalidDataException(Path + ": no table of contents at 0x80");
            var titleCount = BitConverter.ToUInt32(head, 0x84);
            var nd = BitConverter.ToUInt64(head, 0x88);
            var nf = BitConverter.ToUInt64(head, 0x90);
            var stringsOffset = BitConverter.ToUInt64(head, 0x98);
            var stringsSize = BitConverter.ToUInt64(head, 0xA0);
            if (titleCount == 0 || titleCount > 64 || nd > int.MaxValue || nf > int.MaxValue)
                throw new InvalidDataException(Path + ": the table of contents makes no sense");
            dirCount = (uint)nd;
            fileCount = (uint)nf;
            dirsAt = 0xB0 + 0x40L * titleCount;
            filesAt = dirsAt + 24L * dirCount;
            kidsAt = filesAt + 80L * fileCount;
            stringsAt = 0x80 + (long)stringsOffset;
            var indexEnd = stringsAt + (long)stringsSize;
            if (stringsAt < kidsAt || indexEnd > length)
                throw new InvalidDataException(Path + ": the table of contents runs past the end of the file");

            // (the index is mapped, not read: only the pages a lookup touches are read from the disk)
            map = MemoryMappedFile.CreateFromFile(file, null, 0, MemoryMappedFileAccess.Read, HandleInheritability.None, leaveOpen: true);
            index = map.CreateViewAccessor(0, indexEnd, MemoryMappedFileAccess.Read);

            var titles = new List<NupTitle>();
            trees = new (uint, uint, uint)[titleCount];
            for (var i = 0; i < titleCount; i++)
            {
                var at = 0xB0 + 0x40L * i;
                titles.Add(new NupTitle(i, index.ReadUInt64(at), index.ReadUInt32(at + 20)));
                trees[i] = (index.ReadUInt32(at + 8), index.ReadUInt32(at + 12), index.ReadUInt32(at + 16));
            }
            titles.Sort((a, b) => b.Version.CompareTo(a.Version));
            Titles = titles;
        }
        catch
        {
            index?.Dispose();
            map?.Dispose();
            file.Dispose();
            throw;
        }
    }

    public string Describe => string.Format("nup {0} ({1:0.00} GB; titles {2})", Path, length / 1e9, string.Join(", ", Titles));

    // ---- the index ----

    string Name(uint offset, uint count)
    {
        var bytes = new byte[count];
        index.ReadArray(stringsAt + offset, bytes, 0, (int)count);
        return Encoding.UTF8.GetString(bytes);
    }

    string NodeName(uint node) => node < dirCount
        ? Name(index.ReadUInt32(dirsAt + 24L * node), index.ReadUInt32(dirsAt + 24L * node + 4))
        : Name(index.ReadUInt32(filesAt + 80L * (node - dirCount)), index.ReadUInt32(filesAt + 80L * (node - dirCount) + 4));

    /// <summary>A folder's children by name (node indexes).</summary>
    Dictionary<string, uint> Children(uint dir)
    {
        lock (children)
        {
            if (children.TryGetValue(dir, out var known))
                return known;
            var first = index.ReadUInt32(dirsAt + 24L * dir + 8);
            var count = index.ReadUInt32(dirsAt + 24L * dir + 12);
            var kids = new uint[count];
            index.ReadArray(kidsAt + 4L * first, kids, 0, (int)count);
            var names = new Dictionary<string, uint>((int)count, StringComparer.Ordinal);
            foreach (var kid in kids)
                if (kid < dirCount + fileCount)
                    names.TryAdd(NodeName(kid), kid);
            children[dir] = names;
            return names;
        }
    }

    /// <summary>The node a romfs path names in one title's romfs (a dir below dirCount, a file above), or None.</summary>
    uint Find(uint romfsDir, string romfsPath)
    {
        var node = romfsDir;
        var clean = GameSource.Clean(romfsPath);
        if (clean.Length == 0)
            return node;
        foreach (var part in clean.Split('/'))
        {
            if (node >= dirCount || !Children(node).TryGetValue(part, out node))
                return None;
        }
        return node;
    }

    /// <summary>The file record for a romfs path, from the newest title that has it.</summary>
    bool FindFile(string romfsPath, out uint fileIndex)
    {
        foreach (var t in Titles)
        {
            var romfs = trees[t.Index].Romfs;
            if (romfs == None || romfs >= dirCount)
                continue;
            var node = Find(romfs, romfsPath);
            if (node != None && node >= dirCount)
            {
                fileIndex = node - dirCount;
                return true;
            }
        }
        fileIndex = 0;
        return false;
    }

    public bool Exists(string romfsPath) => FindFile(romfsPath, out _);

    public IEnumerable<string> List(string dir)
    {
        var clean = GameSource.Clean(dir);
        var prefix = clean.Length == 0 ? "" : clean + "/";
        var seen = new SortedSet<string>(StringComparer.Ordinal);
        foreach (var t in Titles)
        {
            var romfs = trees[t.Index].Romfs;
            if (romfs == None || romfs >= dirCount)
                continue;
            var node = Find(romfs, clean);
            if (node == None || node >= dirCount)
                continue;
            foreach (var (name, kid) in Children(node))
                seen.Add(prefix + name + (kid < dirCount ? "/" : ""));
        }
        return seen;
    }

    public string? Stamp(string romfsPath)
    {
        if (!FindFile(romfsPath, out var i))
            return null;
        var at = filesAt + 80L * i;
        return string.Format("{0}-{1}-{2}-{3}", length, written.Ticks, index.ReadUInt64(at + 16), index.ReadUInt64(at + 8));
    }

    // ---- the data ----

    public byte[] Read(string romfsPath)
    {
        if (!FindFile(romfsPath, out var i))
            throw new FileNotFoundException("The game has no " + romfsPath + " (" + Path + ")", romfsPath);
        var at = filesAt + 80L * i;
        var size = (long)index.ReadUInt64(at + 8);
        var offset = (long)index.ReadUInt64(at + 16);
        var stored = (long)index.ReadUInt64(at + 24);
        var codec = index.ReadByte(at + 64);
        if (size == 0)
            return Array.Empty<byte>();
        if (size > int.MaxValue || offset < 0 || offset > length)
            throw new InvalidDataException(romfsPath + ": the package's record for it makes no sense");

        if (codec == 1)
        {
            // (the package states the frame's own size: exactly that is read)
            if (stored <= 0 || stored > length - offset)
                throw new InvalidDataException(romfsPath + ": its compressed data runs past the end of the package");
            var frame = ReadAt(offset, (int)stored);
            var output = new byte[size];
            using var z = new Decompressor();
            if (z.Unwrap(frame, output) != size)
                throw new InvalidDataException(romfsPath + ": it unpacked to the wrong size");
            return output;
        }
        if (IsPackedLegacy(romfsPath, offset, size))
            return UnpackLegacy(romfsPath, offset, size);
        if (size > length - offset)
            throw new InvalidDataException(romfsPath + ": its data runs past the end of the package (the .nup is incomplete)");
        return ReadAt(offset, (int)size);
    }

    byte[] ReadAt(long offset, int count)
    {
        var data = new byte[count];
        var done = 0;
        while (done < count)
        {
            var n = RandomAccess.Read(handle, data.AsSpan(done), offset + done);
            if (n <= 0)
                throw new EndOfStreamException("The .nup file ends early: it is incomplete. Copy or download it again.");
            done += n;
        }
        return data;
    }

    /// <summary>The content size a zstd frame header states, or -1 when it states none (nup.py frame_content_size).</summary>
    static long FrameContentSize(ReadOnlySpan<byte> head)
    {
        if (head.Length < 18 || !head[..4].SequenceEqual(ZstdMagic))
            return -1;
        var fhd = head[4];
        var single = (fhd >> 5) & 1;
        var at = 5 + (single != 0 ? 0 : 1) + new[] { 0, 1, 2, 4 }[fhd & 3];
        var n = new[] { single != 0 ? 1 : 0, 2, 4, 8 }[fhd >> 6];
        if (n == 0)
            return -1;
        long size = 0;
        for (var k = n - 1; k >= 0; k--)
            size = size << 8 | head[at + k];
        return n == 2 ? size + 256 : size;
    }

    /// <summary>Older packages (codec 0) may still keep a file as a zstd frame: nup.py is_packed, the same rules. A .zs file
    /// is zstd in the game too; it counts as packed only when what the frame holds is zstd again.</summary>
    bool IsPackedLegacy(string name, long offset, long size)
    {
        var head = new byte[18];
        if (RandomAccess.Read(handle, head, offset) < 18 || !head.AsSpan(0, 4).SequenceEqual(ZstdMagic))
            return false;
        var content = FrameContentSize(head);
        if (name.EndsWith(".zs", StringComparison.Ordinal) && (content < 0 || content == size))
            return content == size && InnerIsZstd(offset, size);
        if (content >= 0)
            return content == size;
        try
        {
            return UnpackLegacy(name, offset, size).Length == size;
        }
        catch (Exception e) when (e is ZstdException or InvalidDataException or IOException)
        {
            return false;
        }
    }

    bool InnerIsZstd(long offset, long size)
    {
        try
        {
            using var s = new DecompressionStream(new WindowStream(handle, offset, Math.Min(size, 1 << 20)));
            var four = new byte[4];
            return s.ReadAtLeast(four, 4, throwOnEndOfStream: false) == 4 && four.AsSpan().SequenceEqual(ZstdMagic);
        }
        catch (Exception e) when (e is ZstdException or IOException)
        {
            return false; // (made with one of the game's dictionaries: the game's own file)
        }
    }

    /// <summary>A frame of unknown length: decompressed until the file's size is reached (the frame cannot be longer than
    /// the file plus 64 KB, as nup.py reads it).</summary>
    byte[] UnpackLegacy(string name, long offset, long size)
    {
        using var s = new DecompressionStream(new WindowStream(handle, offset, Math.Min(size + (1 << 16), length - offset)));
        var output = new byte[size];
        var got = s.ReadAtLeast(output, output.Length, throwOnEndOfStream: false);
        if (got != size)
            throw new InvalidDataException(string.Format("{0}: unpacked to {1} bytes, {2} expected", name, got, size));
        return output;
    }

    /// <summary>A read-only stream over part of the package, read by position (safe beside other threads' reads).</summary>
    sealed class WindowStream(SafeFileHandle handle, long start, long count) : Stream
    {
        long position;
        public override bool CanRead => true;
        public override bool CanSeek => false;
        public override bool CanWrite => false;
        public override long Length => count;
        public override long Position { get => position; set => throw new NotSupportedException(); }
        public override int Read(byte[] buffer, int offset, int n) => Read(buffer.AsSpan(offset, n));
        public override int Read(Span<byte> buffer)
        {
            var n = (int)Math.Min(buffer.Length, count - position);
            if (n <= 0)
                return 0;
            n = RandomAccess.Read(handle, buffer[..n], start + position);
            position += n;
            return n;
        }
        public override void Flush() { }
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int n) => throw new NotSupportedException();
    }

    public void Dispose()
    {
        index.Dispose();
        map.Dispose();
        file.Dispose();
    }
}
