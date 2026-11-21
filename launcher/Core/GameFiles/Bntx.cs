// BNTX: the Switch's texture archive (the game's UI art is in these, inside .bntx.zs files). Offsets as the lab's
// research/scratch/t09_bntx.py reads them, little-endian:
//
//   BNTX header   0x24 u32 texture count, 0x28 u64 offset of the table of BRTI offsets
//   BRTI          0x12 u16 tile mode (0 block-linear, 1 linear), 0x16 u16 mip count, 0x1C u32 format,
//                 0x24 u32 width, 0x28 u32 height, 0x34 u32 block height (log2, in GOBs), 0x50 u32 image size,
//                 0x58 u8[4] channel sources (R, G, B, A: 0 zero, 1 one, 2 red, 3 green, 4 blue, 5 alpha),
//                 0x60 u64 name offset (u16 length, then the name), 0x70 u64 offset of the mip offsets (u64 each)
//
// The format's high byte is the kind (0x0b RGBA8, 0x1a-0x1e BC1-BC5, 0x2d-0x3a ASTC 4x4 to 12x12), the low byte the
// number type (1 unorm, 6 sRGB...). Only mip 0 of the first layer is decoded, as the launcher shows the art at full size.
using System.Buffers.Binary;
using System.Text;

namespace Triforce.Core.GameFiles;

/// <summary>One texture of a BNTX archive.</summary>
public sealed record BntxTexture(string Name, int Width, int Height, uint Format, int MipCount, int TileMode,
    int BlockHeightLog2, long DataOffset, long ImageSize, byte[] ChannelSources)
{
    /// <summary>The format's kind (high byte), e.g. 0x2d for ASTC 4x4.</summary>
    public int Kind => (int)(Format >> 8);

    /// <summary>A readable format name.</summary>
    public string FormatName => Bntx.FormatName(Format);
}

/// <summary>A parsed BNTX archive, with decoding of its textures to RGBA8.</summary>
public sealed class Bntx
{
    readonly byte[] data;
    readonly Dictionary<string, BntxTexture> byName = new(StringComparer.Ordinal);

    public IReadOnlyList<BntxTexture> Textures { get; }

    public Bntx(byte[] data)
    {
        this.data = data;
        if (data.Length < 0x30 || data[0] != 'B' || data[1] != 'N' || data[2] != 'T' || data[3] != 'X')
            throw new InvalidDataException("not a BNTX archive");
        var count = U32(0x24);
        var table = (long)U64(0x28);
        var list = new List<BntxTexture>();
        for (var i = 0; i < count; i++)
        {
            var p = (long)U64(table + 8 * i);
            if (data[p] != 'B' || data[p + 1] != 'R' || data[p + 2] != 'T' || data[p + 3] != 'I')
                throw new InvalidDataException("BNTX texture " + i + " has no BRTI header");
            var nameAt = (long)U64(p + 0x60);
            var nameLength = U16(nameAt);
            var name = Encoding.UTF8.GetString(data, (int)nameAt + 2, nameLength);
            var mips = (long)U64(p + 0x70);
            var tex = new BntxTexture(name, (int)U32(p + 0x24), (int)U32(p + 0x28), U32(p + 0x1C), U16(p + 0x16), U16(p + 0x12),
                (int)(U32(p + 0x34) & 7), (long)U64(mips), U32(p + 0x50), data[(int)(p + 0x58)..(int)(p + 0x5C)]);
            list.Add(tex);
            byName.TryAdd(name, tex);
        }
        Textures = list;
    }

    ushort U16(long at) => BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan((int)at));
    uint U32(long at) => BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan((int)at));
    ulong U64(long at) => BinaryPrimitives.ReadUInt64LittleEndian(data.AsSpan((int)at));

    /// <summary>The texture of that name, or null.</summary>
    public BntxTexture? Find(string name) => byName.GetValueOrDefault(name);

    /// <summary>
    /// A texture's first mip decoded to RGBA8 (Width * Height * 4 bytes). The texels are as stored: an sRGB texture is not
    /// made linear, and a one-channel format (BC4, R8) comes out grey and opaque. With applyChannelSources the texture's own
    /// channel sources are applied as the GPU would (BC4 art in this game is then white, with the channel as alpha).
    /// </summary>
    public byte[] DecodeRgba(BntxTexture t, bool applyChannelSources = false)
    {
        var (blockW, blockH, bytesPerBlock) = BlockInfo(t.Format);
        var wBlocks = (t.Width + blockW - 1) / blockW;
        var hBlocks = (t.Height + blockH - 1) / blockH;
        var source = data.AsSpan((int)t.DataOffset, (int)Math.Min(t.ImageSize, data.Length - t.DataOffset));
        var linear = t.TileMode == 1
            ? source[..(wBlocks * hBlocks * bytesPerBlock)].ToArray()
            : BlockLinear.Deswizzle(source, wBlocks, hBlocks, bytesPerBlock, t.BlockHeightLog2);

        byte[] rgba = (t.Format >> 8) switch
        {
            >= 0x2d and <= 0x3a => Astc.Decompress(linear, t.Width, t.Height, blockW, blockH),
            0x1a => BlockCompression.Decode(linear, t.Width, t.Height, BlockCompression.Kind.BC1),
            0x1b => BlockCompression.Decode(linear, t.Width, t.Height, BlockCompression.Kind.BC2),
            0x1c => BlockCompression.Decode(linear, t.Width, t.Height, BlockCompression.Kind.BC3),
            0x1d => BlockCompression.Decode(linear, t.Width, t.Height, BlockCompression.Kind.BC4),
            0x1e => BlockCompression.Decode(linear, t.Width, t.Height, BlockCompression.Kind.BC5),
            0x0b => linear,
            0x0c => Swap(linear, 2, 1, 0, 3),
            0x02 => Expand(linear, 1),
            0x09 => Expand(linear, 2),
            _ => throw new NotSupportedException(t.Name + ": texture format " + FormatName(t.Format) + " is not supported"),
        };
        if (applyChannelSources)
            ApplySources(rgba, t.ChannelSources);
        return rgba;
    }

    /// <summary>The block size and bytes per block of a format (1x1 for an uncompressed one).</summary>
    static (int W, int H, int Bytes) BlockInfo(uint format) => (format >> 8) switch
    {
        0x02 => (1, 1, 1),
        0x09 => (1, 1, 2),
        0x0b or 0x0c => (1, 1, 4),
        0x1a or 0x1d => (4, 4, 8),
        0x1b or 0x1c or 0x1e => (4, 4, 16),
        var k and >= 0x2d and <= 0x3a => (AstcBlocks[k - 0x2d].W, AstcBlocks[k - 0x2d].H, 16),
        _ => throw new NotSupportedException("texture format " + FormatName(format) + " is not supported"),
    };

    static readonly (int W, int H)[] AstcBlocks =
    {
        (4, 4), (5, 4), (5, 5), (6, 5), (6, 6), (8, 5), (8, 6), (8, 8), (10, 5), (10, 6), (10, 8), (10, 10), (12, 10), (12, 12),
    };

    public static string FormatName(uint format)
    {
        var kind = (int)(format >> 8);
        var type = (format & 0xFF) switch { 1 => "unorm", 2 => "snorm", 5 => "float", 6 => "srgb", var n => "type" + n };
        var name = kind switch
        {
            0x02 => "R8",
            0x09 => "R8G8",
            0x0b => "R8G8B8A8",
            0x0c => "B8G8R8A8",
            >= 0x1a and <= 0x20 => "BC" + (kind - 0x19),
            >= 0x2d and <= 0x3a => "ASTC_" + AstcBlocks[kind - 0x2d].W + "x" + AstcBlocks[kind - 0x2d].H,
            _ => "0x" + kind.ToString("x2"),
        };
        return name + "_" + type + " (0x" + format.ToString("x4") + ")";
    }

    static byte[] Swap(byte[] p, int r, int g, int b, int a)
    {
        var o = new byte[p.Length];
        for (var i = 0; i < p.Length; i += 4)
        {
            o[i] = p[i + r];
            o[i + 1] = p[i + g];
            o[i + 2] = p[i + b];
            o[i + 3] = p[i + a];
        }
        return o;
    }

    /// <summary>R8 to grey (R, R, R, 255); R8G8 to (R, G, 0, 255).</summary>
    static byte[] Expand(byte[] p, int channels)
    {
        var n = p.Length / channels;
        var o = new byte[n * 4];
        for (var i = 0; i < n; i++)
        {
            var r = p[i * channels];
            o[4 * i] = r;
            o[4 * i + 1] = channels == 1 ? r : p[i * channels + 1];
            o[4 * i + 2] = channels == 1 ? r : (byte)0;
            o[4 * i + 3] = 255;
        }
        return o;
    }

    /// <summary>The texture's channel sources applied (0 zero, 1 one, 2-5 red, green, blue, alpha of the decoded texel).</summary>
    static void ApplySources(byte[] rgba, byte[] sources)
    {
        if (sources.Length < 4 || (sources[0] == 2 && sources[1] == 3 && sources[2] == 4 && sources[3] == 5))
            return;
        Span<byte> texel = stackalloc byte[4];
        for (var i = 0; i < rgba.Length; i += 4)
        {
            rgba.AsSpan(i, 4).CopyTo(texel);
            // (one-channel formats decode grey here: their "red" is that grey)
            for (var c = 0; c < 4; c++)
                rgba[i + c] = sources[c] switch { 0 => 0, 1 => 255, >= 2 and <= 5 => texel[sources[c] - 2], _ => texel[c] };
        }
    }
}

/// <summary>BC1-BC5 (DXT1, DXT3, DXT5, RGTC1, RGTC2) decoding to RGBA8. BC4 decodes grey and opaque, BC5 to red and green.</summary>
public static class BlockCompression
{
    public enum Kind { BC1, BC2, BC3, BC4, BC5 }

    public static byte[] Decode(ReadOnlySpan<byte> blocks, int width, int height, Kind kind)
    {
        var bytesPerBlock = kind is Kind.BC1 or Kind.BC4 ? 8 : 16;
        var wBlocks = (width + 3) / 4;
        var hBlocks = (height + 3) / 4;
        var output = new byte[width * height * 4];
        Span<byte> texels = stackalloc byte[64];
        Span<byte> channel = stackalloc byte[16];
        for (var by = 0; by < hBlocks; by++)
            for (var bx = 0; bx < wBlocks; bx++)
            {
                var block = blocks.Slice((by * wBlocks + bx) * bytesPerBlock, bytesPerBlock);
                switch (kind)
                {
                    case Kind.BC1:
                        Color(block, texels, true);
                        break;
                    case Kind.BC2:
                        Color(block[8..], texels, false);
                        for (var i = 0; i < 16; i++)
                        {
                            var nibble = (block[i / 2] >> (4 * (i & 1))) & 15;
                            texels[4 * i + 3] = (byte)(nibble * 17);
                        }
                        break;
                    case Kind.BC3:
                        Color(block[8..], texels, false);
                        Channel(block, channel);
                        for (var i = 0; i < 16; i++)
                            texels[4 * i + 3] = channel[i];
                        break;
                    case Kind.BC4:
                        Channel(block, channel);
                        for (var i = 0; i < 16; i++)
                        {
                            texels[4 * i] = texels[4 * i + 1] = texels[4 * i + 2] = channel[i];
                            texels[4 * i + 3] = 255;
                        }
                        break;
                    case Kind.BC5:
                        Channel(block, channel);
                        for (var i = 0; i < 16; i++)
                            texels[4 * i] = channel[i];
                        Channel(block[8..], channel);
                        for (var i = 0; i < 16; i++)
                        {
                            texels[4 * i + 1] = channel[i];
                            texels[4 * i + 2] = 0;
                            texels[4 * i + 3] = 255;
                        }
                        break;
                }
                for (var y = 0; y < 4 && by * 4 + y < height; y++)
                {
                    var count = Math.Min(4, width - bx * 4);
                    texels.Slice(y * 16, count * 4).CopyTo(output.AsSpan(((by * 4 + y) * width + bx * 4) * 4));
                }
            }
        return output;
    }

    /// <summary>A BC1 color block: two RGB565 endpoints and 2-bit indexes. With punchThrough (BC1), c0 &lt;= c1 selects the
    /// three-color mode whose fourth color is transparent black.</summary>
    static void Color(ReadOnlySpan<byte> b, Span<byte> texels, bool punchThrough)
    {
        var c0 = BinaryPrimitives.ReadUInt16LittleEndian(b);
        var c1 = BinaryPrimitives.ReadUInt16LittleEndian(b[2..]);
        Span<int> pal = stackalloc int[16];
        Rgb565(c0, pal[..4]);
        Rgb565(c1, pal.Slice(4, 4));
        if (c0 > c1 || !punchThrough)
        {
            for (var k = 0; k < 3; k++)
            {
                pal[8 + k] = (2 * pal[k] + pal[4 + k] + 1) / 3;
                pal[12 + k] = (pal[k] + 2 * pal[4 + k] + 1) / 3;
            }
            pal[11] = pal[15] = 255;
        }
        else
        {
            for (var k = 0; k < 3; k++)
            {
                pal[8 + k] = (pal[k] + pal[4 + k] + 1) / 2;
                pal[12 + k] = 0;
            }
            pal[11] = 255;
            pal[15] = 0;
        }
        var bits = BinaryPrimitives.ReadUInt32LittleEndian(b[4..]);
        for (var i = 0; i < 16; i++)
        {
            var idx = (int)(bits >> (2 * i)) & 3;
            for (var k = 0; k < 4; k++)
                texels[4 * i + k] = (byte)pal[4 * idx + k];
        }
    }

    static void Rgb565(ushort c, Span<int> into)
    {
        int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
        into[0] = (r << 3) | (r >> 2);
        into[1] = (g << 2) | (g >> 4);
        into[2] = (b << 3) | (b >> 2);
        into[3] = 255;
    }

    /// <summary>A BC4/BC3-alpha block: two 8-bit endpoints and 3-bit indexes over 6 or 4 interpolated values, rounded
    /// to nearest (as the reference decoder the launcher's art was approved with).</summary>
    static void Channel(ReadOnlySpan<byte> b, Span<byte> values)
    {
        int a0 = b[0], a1 = b[1];
        Span<int> pal = stackalloc int[8];
        pal[0] = a0;
        pal[1] = a1;
        if (a0 > a1)
            for (var k = 1; k <= 6; k++)
                pal[k + 1] = ((7 - k) * a0 + k * a1 + 3) / 7;
        else
        {
            for (var k = 1; k <= 4; k++)
                pal[k + 1] = ((5 - k) * a0 + k * a1 + 2) / 5;
            pal[6] = 0;
            pal[7] = 255;
        }
        ulong bits = 0;
        for (var k = 0; k < 6; k++)
            bits |= (ulong)b[2 + k] << (8 * k);
        for (var i = 0; i < 16; i++)
            values[i] = (byte)pal[(int)(bits >> (3 * i)) & 7];
    }
}
