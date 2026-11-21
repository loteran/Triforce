// A minimal PNG writer and reader for 8-bit RGBA images (the reader is only for comparing with reference images and for
// tests: it takes the PNGs this writer and common tools make, colour type 6 or 2, 8 bits, not interlaced).
using System.Buffers.Binary;
using System.IO.Compression;

namespace Triforce.Core.GameFiles;

public static class Png
{
    static readonly byte[] Signature = { 0x89, (byte)'P', (byte)'N', (byte)'G', 0x0D, 0x0A, 0x1A, 0x0A };

    /// <summary>An RGBA8 image (width * height * 4 bytes, rows top to bottom) as a PNG file's bytes. Each row is filtered
    /// with "Sub", which keeps the zlib step quick and the file reasonably small.</summary>
    public static byte[] Encode(ReadOnlySpan<byte> rgba, int width, int height, CompressionLevel level = CompressionLevel.Fastest)
    {
        if (rgba.Length < (long)width * height * 4)
            throw new ArgumentException("the image is smaller than width * height * 4 bytes");
        var stride = width * 4;
        var raw = new byte[(stride + 1) * height];
        for (var y = 0; y < height; y++)
        {
            var row = rgba.Slice(y * stride, stride);
            var at = y * (stride + 1);
            raw[at] = 1; // (filter: Sub)
            row[..4].CopyTo(raw.AsSpan(at + 1));
            for (var x = 4; x < stride; x++)
                raw[at + 1 + x] = (byte)(row[x] - row[x - 4]);
        }
        using var idat = new MemoryStream();
        using (var z = new ZLibStream(idat, level, leaveOpen: true))
            z.Write(raw);

        using var png = new MemoryStream();
        png.Write(Signature);
        Span<byte> header = stackalloc byte[13];
        BinaryPrimitives.WriteInt32BigEndian(header, width);
        BinaryPrimitives.WriteInt32BigEndian(header[4..], height);
        header[8] = 8;  // bit depth
        header[9] = 6;  // RGBA
        header[10] = 0; // deflate
        header[11] = 0; // adaptive filtering
        header[12] = 0; // not interlaced
        Chunk(png, "IHDR", header);
        Chunk(png, "IDAT", idat.GetBuffer().AsSpan(0, (int)idat.Length));
        Chunk(png, "IEND", ReadOnlySpan<byte>.Empty);
        return png.ToArray();
    }

    static void Chunk(Stream s, string type, ReadOnlySpan<byte> payload)
    {
        Span<byte> four = stackalloc byte[4];
        BinaryPrimitives.WriteInt32BigEndian(four, payload.Length);
        s.Write(four);
        Span<byte> tag = stackalloc byte[4];
        for (var i = 0; i < 4; i++)
            tag[i] = (byte)type[i];
        s.Write(tag);
        s.Write(payload);
        var crc = Crc(Crc(0xFFFFFFFF, tag), payload) ^ 0xFFFFFFFF;
        BinaryPrimitives.WriteUInt32BigEndian(four, crc);
        s.Write(four);
    }

    static readonly uint[] CrcTable = Enumerable.Range(0, 256).Select(n =>
    {
        var c = (uint)n;
        for (var k = 0; k < 8; k++)
            c = (c & 1) != 0 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
        return c;
    }).ToArray();

    /// <summary>The CRC-32 PNG chunks carry (zlib's), continued over more bytes.</summary>
    static uint Crc(uint crc, ReadOnlySpan<byte> bytes)
    {
        foreach (var b in bytes)
            crc = CrcTable[(crc ^ b) & 0xFF] ^ (crc >> 8);
        return crc;
    }

    /// <summary>A PNG decoded to RGBA8 (8-bit RGBA or RGB, not interlaced).</summary>
    public static (int Width, int Height, byte[] Rgba) Decode(byte[] png)
    {
        if (png.Length < 33 || !png.AsSpan(0, 8).SequenceEqual(Signature))
            throw new InvalidDataException("not a PNG file");
        int width = 0, height = 0, channels = 4;
        using var idat = new MemoryStream();
        for (var at = 8; at + 8 <= png.Length;)
        {
            var n = BinaryPrimitives.ReadInt32BigEndian(png.AsSpan(at));
            var type = System.Text.Encoding.ASCII.GetString(png, at + 4, 4);
            var body = png.AsSpan(at + 8, n);
            if (type == "IHDR")
            {
                width = BinaryPrimitives.ReadInt32BigEndian(body);
                height = BinaryPrimitives.ReadInt32BigEndian(body[4..]);
                if (body[8] != 8 || (body[9] != 6 && body[9] != 2) || body[12] != 0)
                    throw new NotSupportedException("only 8-bit RGB/RGBA PNGs without interlacing are read");
                channels = body[9] == 6 ? 4 : 3;
            }
            else if (type == "IDAT")
                idat.Write(body);
            else if (type == "IEND")
                break;
            at += 12 + n;
        }
        idat.Position = 0;
        var stride = width * channels;
        var raw = new byte[(stride + 1) * height];
        using (var z = new ZLibStream(idat, CompressionMode.Decompress))
            z.ReadExactly(raw);

        var pixels = new byte[stride * height];
        for (var y = 0; y < height; y++)
        {
            var filter = raw[y * (stride + 1)];
            var src = raw.AsSpan(y * (stride + 1) + 1, stride);
            var dst = pixels.AsSpan(y * stride, stride);
            var up = y > 0 ? pixels.AsSpan((y - 1) * stride, stride) : new byte[stride];
            for (var x = 0; x < stride; x++)
            {
                int a = x >= channels ? dst[x - channels] : 0, b = up[x], c = x >= channels ? up[x - channels] : 0;
                dst[x] = (byte)(src[x] + filter switch
                {
                    0 => 0,
                    1 => a,
                    2 => b,
                    3 => (a + b) / 2,
                    4 => Paeth(a, b, c),
                    _ => throw new InvalidDataException("PNG filter " + filter),
                });
            }
        }
        if (channels == 4)
            return (width, height, pixels);
        var rgba = new byte[width * height * 4];
        for (var i = 0; i < width * height; i++)
        {
            pixels.AsSpan(i * 3, 3).CopyTo(rgba.AsSpan(i * 4));
            rgba[i * 4 + 3] = 255;
        }
        return (width, height, rgba);
    }

    static int Paeth(int a, int b, int c)
    {
        int p = a + b - c, pa = Math.Abs(p - a), pb = Math.Abs(p - b), pc = Math.Abs(p - c);
        return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
    }
}
