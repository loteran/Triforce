// SPDX-FileCopyrightText: Copyright 2025 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// (Triforce) The Tegra GPU's block-linear texture layout undone, ported from suyu's video_core/textures/decoders.cpp
// (SwizzleImpl with TO_LINEAR false, depth 1). A texture is stored in GOBs (64 bytes by 8 rows, 512 bytes), stacked
// 2^blockHeight GOBs tall into blocks; blocks run left to right, then the next row of blocks.
//
// Inside a GOB this is the Switch 2's order, as in our suyu's decoders.h: the Tegra X1's (x bits 0-3, 5, 8 / y bits 4,
// 6, 7) with offset bits 5 and 6 swapped, so x bit 4 lands on bit 6 and y bit 1 on bit 5. (The X1's order, which most
// Switch tools use, scrambles this game's textures in 4x4-block patches.)
namespace Triforce.Core.GameFiles;

/// <summary>Tegra block-linear to linear.</summary>
public static class BlockLinear
{
    const int GobSizeXShift = 6;   // 64 bytes
    const int GobSizeYShift = 3;   // 8 rows
    const int GobSizeShift = GobSizeXShift + GobSizeYShift;   // 512 bytes

    /// <summary>The position of a byte inside its GOB: pdep of x into SWIZZLE_X_BITS 0b1_0100_1111 and of y into
    /// SWIZZLE_Y_BITS 0b0_1011_0000.</summary>
    static int SwizzleX(int x) => (x & 15) | (x & 16) << 2 | (x & 32) << 3;
    static int SwizzleY(int y) => (y & 3) << 4 | (y & 4) << 5;

    /// <summary>The size a block-linear image takes, in bytes.</summary>
    public static long Size(int width, int height, int bytesPerElement, int blockHeightLog2)
    {
        var alignedWidth = (width * bytesPerElement + 63) & ~63;
        var rows = 1 << (GobSizeYShift + blockHeightLog2);
        var alignedHeight = (height + rows - 1) / rows * rows;
        return (long)alignedWidth * alignedHeight;
    }

    /// <summary>
    /// A block-linear image made linear. width and height count elements (pixels, or blocks of a compressed format);
    /// bytesPerElement is 1, 2, 4, 8 or 16. The result is width * height * bytesPerElement bytes, rows one after another.
    /// </summary>
    public static byte[] Deswizzle(ReadOnlySpan<byte> input, int width, int height, int bytesPerElement, int blockHeightLog2)
    {
        if (bytesPerElement is not (1 or 2 or 4 or 8 or 16))
            throw new NotSupportedException(bytesPerElement + " bytes per element");
        var pitch = width * bytesPerElement;
        var output = new byte[pitch * height];
        var gobsInX = (pitch + 63) >> GobSizeXShift;
        var blockSize = gobsInX << (GobSizeShift + blockHeightLog2);
        var blockHeightMask = (1 << blockHeightLog2) - 1;
        var xShift = GobSizeShift + blockHeightLog2;

        if (Size(width, height, bytesPerElement, blockHeightLog2) > input.Length)
            throw new InvalidDataException("the texture's data is shorter than its size calls for");

        for (var y = 0; y < height; y++)
        {
            var swizzledY = SwizzleY(y);
            var blockY = y >> GobSizeYShift;
            var offsetY = (blockY >> blockHeightLog2) * blockSize + ((blockY & blockHeightMask) << GobSizeShift);
            var row = output.AsSpan(y * pitch, pitch);
            for (var x = 0; x < pitch; x += bytesPerElement)
            {
                var offsetX = (x >> GobSizeXShift) << xShift;
                var at = offsetY + offsetX + (SwizzleX(x & 63) | swizzledY);
                input.Slice(at, bytesPerElement).CopyTo(row.Slice(x, bytesPerElement));
            }
        }
        return output;
    }
}
