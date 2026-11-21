// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 The University of North Carolina at Chapel Hill
// SPDX-License-Identifier: Apache-2.0

// Please send all BUG REPORTS to <pavel@cs.unc.edu>.
// <http://gamma.cs.unc.edu/FasTC/>

// (Triforce) A C# port of suyu's video_core/textures/astc.cpp (itself FasTC's decoder): ASTC blocks of any 2D size from
// 4x4 to 12x12, every LDR color endpoint mode, and the HDR modes suyu handles (clamped to bytes). The steps, names and
// arithmetic follow the C++ one for one, so a difference from suyu's output is a porting bug. Output is RGBA8, the
// endpoint values as stored (an sRGB texture's bytes are not converted to linear: the PNG shows them as the game's art).
using System.Numerics;

namespace Triforce.Core.GameFiles;

/// <summary>ASTC texture decoding to RGBA8.</summary>
public static class Astc
{
    // ---- bit streams ----

    /// <summary>Reads bits from the low bit of the first byte upward; past the end it reads zeros.</summary>
    ref struct InputBitStream
    {
        readonly ReadOnlySpan<byte> data;
        int bitsRead;

        public InputBitStream(ReadOnlySpan<byte> data) { this.data = data; bitsRead = 0; }

        public readonly int BitsRead => bitsRead;

        public uint ReadBit()
        {
            if (bitsRead >= data.Length * 8)
                return 0;
            var bit = (uint)(data[bitsRead >> 3] >> (bitsRead & 7)) & 1;
            bitsRead++;
            return bit;
        }

        public uint ReadBits(int n)
        {
            uint ret = 0;
            for (var i = 0; i < n; i++)
                ret |= ReadBit() << i;
            return ret;
        }
    }

    /// <summary>Writes bits from the low bit of the first byte upward, at most `bits` of them.</summary>
    ref struct OutputBitStream
    {
        readonly Span<byte> data;
        readonly int numBits;
        int bitsWritten;

        public OutputBitStream(Span<byte> data, int bits) { this.data = data; numBits = bits; bitsWritten = 0; }

        public void WriteBits(uint val, int n)
        {
            for (var i = 0; i < n; i++)
                WriteBit(((val >> i) & 1) != 0);
        }

        void WriteBit(bool b)
        {
            if (bitsWritten >= numBits)
                return;
            var mask = (byte)(1 << (bitsWritten & 7));
            data[bitsWritten >> 3] &= (byte)~mask;
            if (b)
                data[bitsWritten >> 3] |= mask;
            bitsWritten++;
        }
    }

    /// <summary>Bit `pos` of v (Bits&lt;T&gt;::operator[]).</summary>
    static uint Bit(uint v, int pos) => (v >> pos) & 1;

    /// <summary>Bits start..end of v, inclusive (Bits&lt;T&gt;::operator()).</summary>
    static uint Range(uint v, int start, int end)
    {
        if (start == end)
            return Bit(v, start);
        if (start > end)
            (start, end) = (end, start);
        var mask = (1u << (end - start + 1)) - 1;
        return (v >> start) & mask;
    }

    // ---- bounded integer sequences ----

    enum IntegerEncoding : byte { JustBits, Quint, Trit }

    struct IntegerEncodedValue
    {
        public IntegerEncoding Encoding;
        public int NumBits;
        public uint BitValue;
        public uint QuintOrTritValue;

        public IntegerEncodedValue(IntegerEncoding encoding, int numBits)
        {
            Encoding = encoding;
            NumBits = numBits;
            BitValue = 0;
            QuintOrTritValue = 0;
        }

        public readonly bool MatchesEncoding(IntegerEncodedValue other) => Encoding == other.Encoding && NumBits == other.NumBits;

        /// <summary>The number of bits needed to encode numVals values.</summary>
        public readonly int GetBitLength(int numVals)
        {
            var total = NumBits * numVals;
            if (Encoding == IntegerEncoding.Trit)
                total += (numVals * 8 + 4) / 5;
            else if (Encoding == IntegerEncoding.Quint)
                total += (numVals * 7 + 2) / 3;
            return total;
        }
    }

    /// <summary>The encoding of values that can take no more than maxValue values.</summary>
    static IntegerEncodedValue CreateEncoding(uint maxValue)
    {
        while (maxValue > 0)
        {
            var check = maxValue + 1;
            // (a power of two?)
            if ((check & (check - 1)) == 0)
                return new IntegerEncodedValue(IntegerEncoding.JustBits, BitOperations.PopCount(maxValue));
            // (of the form 3*2^n - 1?)
            if (check % 3 == 0 && ((check / 3) & ((check / 3) - 1)) == 0)
                return new IntegerEncodedValue(IntegerEncoding.Trit, BitOperations.PopCount(check / 3 - 1));
            // (of the form 5*2^n - 1?)
            if (check % 5 == 0 && ((check / 5) & ((check / 5) - 1)) == 0)
                return new IntegerEncodedValue(IntegerEncoding.Quint, BitOperations.PopCount(check / 5 - 1));
            // (no bounded integer sequence can: try one less)
            maxValue--;
        }
        return new IntegerEncodedValue(IntegerEncoding.JustBits, 0);
    }

    static readonly IntegerEncodedValue[] Encodings = MakeEncodedValues();

    static IntegerEncodedValue[] MakeEncodedValues()
    {
        var e = new IntegerEncodedValue[256];
        for (var i = 0; i < e.Length; i++)
            e[i] = CreateEncoding((uint)i);
        return e;
    }

    /// <summary>A fixed-capacity list of decoded values (boost::static_vector without throwing: values past the capacity
    /// are dropped, as there).</summary>
    sealed class EncodedVector
    {
        public readonly IntegerEncodedValue[] Items = new IntegerEncodedValue[300];
        public int Count;
        public void Clear() => Count = 0;
        public void Add(IntegerEncodedValue v)
        {
            if (Count < Items.Length)
                Items[Count++] = v;
        }
    }

    static void DecodeTritBlock(ref InputBitStream bits, EncodedVector result, int nBitsPerValue)
    {
        // (section C.2.12: read the trit-encoded block per table C.2.14)
        Span<uint> m = stackalloc uint[5];
        Span<uint> t = stackalloc uint[5];
        m[0] = bits.ReadBits(nBitsPerValue);
        var T = bits.ReadBits(2);
        m[1] = bits.ReadBits(nBitsPerValue);
        T |= bits.ReadBits(2) << 2;
        m[2] = bits.ReadBits(nBitsPerValue);
        T |= bits.ReadBit() << 4;
        m[3] = bits.ReadBits(nBitsPerValue);
        T |= bits.ReadBits(2) << 5;
        m[4] = bits.ReadBits(nBitsPerValue);
        T |= bits.ReadBit() << 7;

        uint C;
        if (Range(T, 2, 4) == 7)
        {
            C = (Range(T, 5, 7) << 2) | Range(T, 0, 1);
            t[4] = t[3] = 2;
        }
        else
        {
            C = Range(T, 0, 4);
            if (Range(T, 5, 6) == 3)
            {
                t[4] = 2;
                t[3] = Bit(T, 7);
            }
            else
            {
                t[4] = Bit(T, 7);
                t[3] = Range(T, 5, 6);
            }
        }

        if (Range(C, 0, 1) == 3)
        {
            t[2] = 2;
            t[1] = Bit(C, 4);
            t[0] = (Bit(C, 3) << 1) | (Bit(C, 2) & ~Bit(C, 3));
        }
        else if (Range(C, 2, 3) == 3)
        {
            t[2] = 2;
            t[1] = 2;
            t[0] = Range(C, 0, 1);
        }
        else
        {
            t[2] = Bit(C, 4);
            t[1] = Range(C, 2, 3);
            t[0] = (Bit(C, 1) << 1) | (Bit(C, 0) & ~Bit(C, 1));
        }

        for (var i = 0; i < 5; i++)
        {
            var val = new IntegerEncodedValue(IntegerEncoding.Trit, nBitsPerValue) { BitValue = m[i], QuintOrTritValue = t[i] };
            result.Add(val);
        }
    }

    static void DecodeQuintBlock(ref InputBitStream bits, EncodedVector result, int nBitsPerValue)
    {
        // (section C.2.12: read the quint-encoded block per table C.2.15)
        Span<uint> m = stackalloc uint[3];
        Span<uint> q = stackalloc uint[3];
        m[0] = bits.ReadBits(nBitsPerValue);
        var Q = bits.ReadBits(3);
        m[1] = bits.ReadBits(nBitsPerValue);
        Q |= bits.ReadBits(2) << 3;
        m[2] = bits.ReadBits(nBitsPerValue);
        Q |= bits.ReadBits(2) << 5;

        if (Range(Q, 1, 2) == 3 && Range(Q, 5, 6) == 0)
        {
            q[0] = q[1] = 4;
            q[2] = (Bit(Q, 0) << 2) | ((Bit(Q, 4) & ~Bit(Q, 0)) << 1) | (Bit(Q, 3) & ~Bit(Q, 0));
        }
        else
        {
            uint C;
            if (Range(Q, 1, 2) == 3)
            {
                q[2] = 4;
                C = (Range(Q, 3, 4) << 3) | ((~Range(Q, 5, 6) & 3) << 1) | Bit(Q, 0);
            }
            else
            {
                q[2] = Range(Q, 5, 6);
                C = Range(Q, 0, 4);
            }

            if (Range(C, 0, 2) == 5)
            {
                q[1] = 4;
                q[0] = Range(C, 3, 4);
            }
            else
            {
                q[1] = Range(C, 3, 4);
                q[0] = Range(C, 0, 2);
            }
        }

        for (var i = 0; i < 3; i++)
        {
            var val = new IntegerEncodedValue(IntegerEncoding.Quint, nBitsPerValue) { BitValue = m[i], QuintOrTritValue = q[i] };
            result.Add(val);
        }
    }

    /// <summary>Fills result with nValues values of at most maxRange from the bit stream.</summary>
    static void DecodeIntegerSequence(EncodedVector result, ref InputBitStream bits, uint maxRange, int nValues)
    {
        var val = Encodings[maxRange];
        var decoded = 0;
        while (decoded < nValues)
        {
            switch (val.Encoding)
            {
                case IntegerEncoding.Quint:
                    DecodeQuintBlock(ref bits, result, val.NumBits);
                    decoded += 3;
                    break;
                case IntegerEncoding.Trit:
                    DecodeTritBlock(ref bits, result, val.NumBits);
                    decoded += 5;
                    break;
                case IntegerEncoding.JustBits:
                    val.BitValue = bits.ReadBits(val.NumBits);
                    result.Add(val);
                    decoded++;
                    break;
            }
        }
    }

    // ---- block mode ----

    struct TexelWeightParams
    {
        public uint Width, Height;
        public bool DualPlane;
        public uint MaxWeight;
        public bool Error, VoidExtentLDR, VoidExtentHDR;

        public readonly int GetPackedBitSize()
        {
            var n = (int)(Height * Width);
            if (DualPlane)
                n *= 2;
            return Encodings[MaxWeight].GetBitLength(n);
        }

        public readonly int GetNumWeightValues()
        {
            var n = (int)(Width * Height);
            if (DualPlane)
                n *= 2;
            return n;
        }
    }

    static TexelWeightParams DecodeBlockInfo(ref InputBitStream strm)
    {
        var p = new TexelWeightParams();
        // (the whole block mode at once)
        var modeBits = strm.ReadBits(11);

        // (the void extent block mode?)
        if ((modeBits & 0x01FF) == 0x1FC)
        {
            if ((modeBits & 0x200) != 0)
                p.VoidExtentHDR = true;
            else
                p.VoidExtentLDR = true;
            // (the next two bits must be one)
            if ((modeBits & 0x400) == 0 || strm.ReadBit() == 0)
                p.Error = true;
            return p;
        }

        // (the last four bits zero: reserved)
        if ((modeBits & 0xF) == 0)
        {
            p.Error = true;
            return p;
        }

        // (the last two bits zero and bits 6-8 all one: reserved too)
        if ((modeBits & 0x3) == 0 && (modeBits & 0x1C0) == 0x1C0)
        {
            p.Error = true;
            return p;
        }

        // (the layout: a number 0-9 per table C.2.8 of the ASTC spec)
        uint layout;
        if ((modeBits & 0x1) != 0 || (modeBits & 0x2) != 0)
        {
            if ((modeBits & 0x8) != 0)
                layout = (modeBits & 0x4) != 0 ? ((modeBits & 0x100) != 0 ? 4u : 3u) : 2u;
            else
                layout = (modeBits & 0x4) != 0 ? 1u : 0u;
        }
        else
        {
            if ((modeBits & 0x100) != 0)
                layout = (modeBits & 0x80) != 0 ? ((modeBits & 0x20) != 0 ? 8u : 7u) : 9u;
            else
                layout = (modeBits & 0x80) != 0 ? 6u : 5u;
        }

        // (R: the weight range)
        var R = (modeBits & 0x10) != 0 ? 1u : 0u;
        if (layout < 5)
            R |= (modeBits & 0x3) << 1;
        else
            R |= (modeBits & 0xC) >> 1;

        // (the weight grid's width and height)
        switch (layout)
        {
            case 0: p.Width = ((modeBits >> 7) & 0x3) + 4; p.Height = ((modeBits >> 5) & 0x3) + 2; break;
            case 1: p.Width = ((modeBits >> 7) & 0x3) + 8; p.Height = ((modeBits >> 5) & 0x3) + 2; break;
            case 2: p.Width = ((modeBits >> 5) & 0x3) + 2; p.Height = ((modeBits >> 7) & 0x3) + 8; break;
            case 3: p.Width = ((modeBits >> 5) & 0x3) + 2; p.Height = ((modeBits >> 7) & 0x1) + 6; break;
            case 4: p.Width = ((modeBits >> 7) & 0x1) + 2; p.Height = ((modeBits >> 5) & 0x3) + 2; break;
            case 5: p.Width = 12; p.Height = ((modeBits >> 5) & 0x3) + 2; break;
            case 6: p.Width = ((modeBits >> 5) & 0x3) + 2; p.Height = 12; break;
            case 7: p.Width = 6; p.Height = 10; break;
            case 8: p.Width = 10; p.Height = 6; break;
            case 9: p.Width = ((modeBits >> 5) & 0x3) + 6; p.Height = ((modeBits >> 9) & 0x3) + 6; break;
            default: p.Error = true; break;
        }

        // (dual planes and/or high precision)
        var D = layout != 9 && (modeBits & 0x400) != 0;
        var H = layout != 9 && (modeBits & 0x200) != 0;
        if (R < 2 || R > 7)
        {
            p.Error = true;
            return p;
        }
        p.MaxWeight = H ? MaxWeightsHigh[R - 2] : MaxWeightsLow[R - 2];
        p.DualPlane = D;
        return p;
    }

    static readonly uint[] MaxWeightsHigh = { 9, 11, 15, 19, 23, 31 };
    static readonly uint[] MaxWeightsLow = { 1, 2, 3, 4, 5, 7 };

    // ---- bit replication ----

    /// <summary>The low numBits of val repeated down to toBit bits.</summary>
    static uint Replicate(uint val, int numBits, int toBit)
    {
        if (numBits == 0 || toBit == 0)
            return 0;
        var v = val & ((1u << numBits) - 1);
        var res = v;
        var reslen = numBits;
        while (reslen < toBit)
        {
            var comp = 0;
            if (numBits > toBit - reslen)
            {
                var newshift = toBit - reslen;
                comp = numBits - newshift;
                numBits = newshift;
            }
            res <<= numBits;
            res |= v >> comp;
            reslen += numBits;
        }
        return res;
    }

    static readonly uint[] ReplicateByteTo16Table = MakeTable(8, 16);
    static readonly uint[] ReplicateBitTo7Table = MakeTable(1, 7);
    static readonly uint[] ReplicateBitTo9Table = MakeTable(1, 9);
    static readonly uint[][] ReplicateTo8Tables = Enumerable.Range(0, 9).Select(n => n == 0 ? Array.Empty<uint>() : MakeTable(n, 8)).ToArray();
    static readonly uint[][] ReplicateTo6Tables = Enumerable.Range(0, 6).Select(n => n == 0 ? Array.Empty<uint>() : MakeTable(n, 6)).ToArray();

    static uint[] MakeTable(int numBits, int toBit)
    {
        var t = new uint[1 << numBits];
        for (var i = 0u; i < t.Length; i++)
            t[i] = Replicate(i, numBits, toBit);
        return t;
    }

    static uint FastReplicateTo8(uint value, int numBits) =>
        numBits >= 1 && numBits <= 8 ? ReplicateTo8Tables[numBits][value & ((1u << numBits) - 1)] : Replicate(value, numBits, 8);

    static uint FastReplicateTo6(uint value, int numBits) =>
        numBits >= 1 && numBits <= 5 ? ReplicateTo6Tables[numBits][value & ((1u << numBits) - 1)] : Replicate(value, numBits, 6);

    // ---- pixels ----

    /// <summary>A color: alpha, red, green, blue as signed 16-bit values (Pixel; the bit depth is always 8 where this
    /// decoder makes them, so packing only keeps the low bytes).</summary>
    struct Pixel
    {
        public short A, R, G, B;

        public Pixel(int a, int r, int g, int b)
        {
            A = (short)a;
            R = (short)r;
            G = (short)g;
            B = (short)b;
        }

        public readonly int Component(int i) => i switch { 0 => A, 1 => R, 2 => G, _ => B };

        public void SetComponent(int i, int v)
        {
            switch (i)
            {
                case 0: A = (short)v; break;
                case 1: R = (short)v; break;
                case 2: G = (short)v; break;
                default: B = (short)v; break;
            }
        }

        /// <summary>Each component clamped to 0..255.</summary>
        public void ClampByte()
        {
            A = Clamp(A);
            R = Clamp(R);
            G = Clamp(G);
            B = Clamp(B);
        }

        static short Clamp(short v) => v < 0 ? (short)0 : v > 255 ? (short)255 : v;

        /// <summary>R in the low byte, then G, B, A.</summary>
        public readonly uint Pack()
        {
            uint r = (uint)A & 0xFFFF;
            r <<= 8;
            r |= (uint)B & 0xFFFF;
            r <<= 8;
            r |= (uint)G & 0xFFFF;
            r <<= 8;
            r |= (uint)R & 0xFFFF;
            return r;
        }
    }

    // ---- color endpoints ----

    static void DecodeColorValues(Span<uint> output, ReadOnlySpan<byte> data, ReadOnlySpan<uint> modes, int nPartitions,
        int nBitsForColorData, EncodedVector decoded)
    {
        // (how many color values there are)
        var nValues = 0;
        for (var i = 0; i < nPartitions; i++)
            nValues += (int)(((modes[i] >> 2) + 1) << 1);

        // (from that and the bits there are, the largest range each value can have)
        var range = 256u;
        while (--range > 0)
        {
            var val = Encodings[range];
            var bitLength = val.GetBitLength(nValues);
            if (bitLength <= nBitsForColorData)
            {
                // (the smallest range with the same encoding)
                while (--range > 0)
                {
                    if (!Encodings[range].MatchesEncoding(val))
                        break;
                }
                range++;
                break;
            }
        }

        decoded.Clear();
        var colorStream = new InputBitStream(data);
        DecodeIntegerSequence(decoded, ref colorStream, range, nValues);

        // (the values dequantized to 0-255: ASTC spec C.2.13)
        var outIdx = 0;
        for (var k = 0; k < decoded.Count; k++)
        {
            if (outIdx >= nValues)
                break;
            var val = decoded.Items[k];
            var bitlen = val.NumBits;
            var bitval = val.BitValue;

            uint A, B = 0, C = 0, D = 0;
            // (A: the low bit replicated 9 times)
            A = ReplicateBitTo9Table[bitval & 1];

            switch (val.Encoding)
            {
                case IntegerEncoding.JustBits:
                    output[outIdx++] = FastReplicateTo8(bitval, bitlen);
                    break;

                case IntegerEncoding.Trit:
                {
                    D = val.QuintOrTritValue;
                    switch (bitlen)
                    {
                        case 1: C = 204; break;
                        case 2:
                        {
                            C = 93;
                            var b = (bitval >> 1) & 1; // B = b000b0bb0
                            B = (b << 8) | (b << 4) | (b << 2) | (b << 1);
                            break;
                        }
                        case 3:
                        {
                            C = 44;
                            var cb = (bitval >> 1) & 3; // B = cb000cbcb
                            B = (cb << 7) | (cb << 2) | cb;
                            break;
                        }
                        case 4:
                        {
                            C = 22;
                            var dcb = (bitval >> 1) & 7; // B = dcb000dcb
                            B = (dcb << 6) | dcb;
                            break;
                        }
                        case 5:
                        {
                            C = 11;
                            var edcb = (bitval >> 1) & 0xF; // B = edcb000ed
                            B = (edcb << 5) | (edcb >> 2);
                            break;
                        }
                        case 6:
                        {
                            C = 5;
                            var fedcb = (bitval >> 1) & 0x1F; // B = fedcb000f
                            B = (fedcb << 4) | (fedcb >> 4);
                            break;
                        }
                    }
                    break;
                }

                case IntegerEncoding.Quint:
                {
                    D = val.QuintOrTritValue;
                    switch (bitlen)
                    {
                        case 1: C = 113; break;
                        case 2:
                        {
                            C = 54;
                            var b = (bitval >> 1) & 1; // B = b0000bb00
                            B = (b << 8) | (b << 3) | (b << 2);
                            break;
                        }
                        case 3:
                        {
                            C = 26;
                            var cb = (bitval >> 1) & 3; // B = cb0000cbc
                            B = (cb << 7) | (cb << 1) | (cb >> 1);
                            break;
                        }
                        case 4:
                        {
                            C = 13;
                            var dcb = (bitval >> 1) & 7; // B = dcb0000dc
                            B = (dcb << 6) | (dcb >> 1);
                            break;
                        }
                        case 5:
                        {
                            C = 6;
                            var edcb = (bitval >> 1) & 0xF; // B = edcb0000e
                            B = (edcb << 5) | (edcb >> 3);
                            break;
                        }
                    }
                    break;
                }
            }

            if (val.Encoding != IntegerEncoding.JustBits)
            {
                var T = D * C + B;
                T ^= A;
                T = (A & 0x80) | (T >> 2);
                output[outIdx++] = T;
            }
        }
    }

    static uint UnquantizeTexelWeight(IntegerEncodedValue val)
    {
        var bitval = val.BitValue;
        var bitlen = val.NumBits;
        var A = ReplicateBitTo7Table[bitval & 1];
        uint B = 0, C = 0, D = 0;
        uint result = 0;

        switch (val.Encoding)
        {
            case IntegerEncoding.JustBits:
                result = FastReplicateTo6(bitval, bitlen);
                break;

            case IntegerEncoding.Trit:
                D = val.QuintOrTritValue;
                switch (bitlen)
                {
                    case 0: result = D switch { 0 => 0u, 1 => 32u, _ => 63u }; break;
                    case 1: C = 50; break;
                    case 2:
                    {
                        C = 23;
                        var b = (bitval >> 1) & 1;
                        B = (b << 6) | (b << 2) | b;
                        break;
                    }
                    case 3:
                    {
                        C = 11;
                        var cb = (bitval >> 1) & 3;
                        B = (cb << 5) | cb;
                        break;
                    }
                }
                break;

            case IntegerEncoding.Quint:
                D = val.QuintOrTritValue;
                switch (bitlen)
                {
                    case 0: result = D switch { 0 => 0u, 1 => 16u, 2 => 32u, 3 => 47u, _ => 63u }; break;
                    case 1: C = 28; break;
                    case 2:
                    {
                        C = 13;
                        var b = (bitval >> 1) & 1;
                        B = (b << 6) | (b << 1);
                        break;
                    }
                }
                break;
        }

        if (val.Encoding != IntegerEncoding.JustBits && bitlen > 0)
        {
            result = D * C + B;
            result ^= A;
            result = (A & 0x20) | (result >> 2);
        }

        // (from 0..63 to 0..64)
        if (result > 32)
            result += 1;
        return result;
    }

    /// <summary>The weights unquantized and spread from the weight grid over the block (section C.2.18).</summary>
    static void UnquantizeTexelWeights(uint[][] output, EncodedVector weights, in TexelWeightParams p, int blockWidth,
        int blockHeight, uint[][] unquantized)
    {
        var weightIdx = 0;
        var gridSize = (int)(p.Width * p.Height);
        for (var k = 0; k < weights.Count; k++)
        {
            unquantized[0][weightIdx] = UnquantizeTexelWeight(weights.Items[k]);
            if (p.DualPlane)
            {
                ++k;
                // (the C++ reads the next value before it checks for the end: a value past the end is whatever was there)
                unquantized[1][weightIdx] = UnquantizeTexelWeight(weights.Items[Math.Min(k, weights.Items.Length - 1)]);
                if (k == weights.Count)
                    break;
            }
            if (++weightIdx >= gridSize)
                break;
        }

        var Ds = (uint)((1024 + blockWidth / 2) / (blockWidth - 1));
        var Dt = (uint)((1024 + blockHeight / 2) / (blockHeight - 1));
        var planes = p.DualPlane ? 2 : 1;
        for (var plane = 0; plane < planes; plane++)
        {
            var u = unquantized[plane];
            for (var t = 0; t < blockHeight; t++)
                for (var s = 0; s < blockWidth; s++)
                {
                    var cs = Ds * (uint)s;
                    var ct = Dt * (uint)t;
                    var gs = (cs * (p.Width - 1) + 32) >> 6;
                    var gt = (ct * (p.Height - 1) + 32) >> 6;
                    var js = gs >> 4;
                    var fs = gs & 0xF;
                    var jt = gt >> 4;
                    var ft = gt & 0x0F;
                    var w11 = (fs * ft + 8) >> 4;
                    var w10 = ft - w11;
                    var w01 = fs - w11;
                    var w00 = 16 - fs - ft + w11;
                    var v0 = js + jt * p.Width;
                    uint P(uint idx) => idx < (uint)gridSize ? u[idx] : 0;
                    var p00 = P(v0);
                    var p01 = P(v0 + 1);
                    var p10 = P(v0 + p.Width);
                    var p11 = P(v0 + p.Width + 1);
                    output[plane][t * blockWidth + s] = (p00 * w00 + p01 * w01 + p10 * w10 + p11 * w11 + 8) >> 4;
                }
        }
    }

    /// <summary>Moves a bit as described in C.2.14.</summary>
    static void BitTransferSigned(ref int a, ref int b)
    {
        b >>= 1;
        b |= a & 0x80;
        a >>= 1;
        a &= 0x3F;
        if ((a & 0x20) != 0)
            a -= 0x40;
    }

    /// <summary>More precision for blue (C.2.14).</summary>
    static Pixel BlueContract(int a, int r, int g, int b) => new((short)a, (short)((r + b) >> 1), (short)((g + b) >> 1), (short)b);

    // ---- partitions (C.2.21) ----

    static uint Hash52(uint p)
    {
        p ^= p >> 15;
        p -= p << 17;
        p += p << 7;
        p += p << 4;
        p ^= p >> 5;
        p += p << 16;
        p ^= p >> 7;
        p ^= p >> 3;
        p ^= p << 6;
        p ^= p >> 17;
        return p;
    }

    static int SelectPartition(int seed, int x, int y, int z, int partitionCount, bool smallBlock)
    {
        if (partitionCount == 1)
            return 0;
        if (smallBlock)
        {
            x <<= 1;
            y <<= 1;
            z <<= 1;
        }
        seed += (partitionCount - 1) * 1024;

        var rnum = Hash52((uint)seed);
        var seed1 = (byte)(rnum & 0xF);
        var seed2 = (byte)((rnum >> 4) & 0xF);
        var seed3 = (byte)((rnum >> 8) & 0xF);
        var seed4 = (byte)((rnum >> 12) & 0xF);
        var seed5 = (byte)((rnum >> 16) & 0xF);
        var seed6 = (byte)((rnum >> 20) & 0xF);
        var seed7 = (byte)((rnum >> 24) & 0xF);
        var seed8 = (byte)((rnum >> 28) & 0xF);
        var seed9 = (byte)((rnum >> 18) & 0xF);
        var seed10 = (byte)((rnum >> 22) & 0xF);
        var seed11 = (byte)((rnum >> 26) & 0xF);
        var seed12 = (byte)(((rnum >> 30) | (rnum << 2)) & 0xF);

        seed1 = (byte)(seed1 * seed1);
        seed2 = (byte)(seed2 * seed2);
        seed3 = (byte)(seed3 * seed3);
        seed4 = (byte)(seed4 * seed4);
        seed5 = (byte)(seed5 * seed5);
        seed6 = (byte)(seed6 * seed6);
        seed7 = (byte)(seed7 * seed7);
        seed8 = (byte)(seed8 * seed8);
        seed9 = (byte)(seed9 * seed9);
        seed10 = (byte)(seed10 * seed10);
        seed11 = (byte)(seed11 * seed11);
        seed12 = (byte)(seed12 * seed12);

        int sh1, sh2;
        if ((seed & 1) != 0)
        {
            sh1 = (seed & 2) != 0 ? 4 : 5;
            sh2 = partitionCount == 3 ? 6 : 5;
        }
        else
        {
            sh1 = partitionCount == 3 ? 6 : 5;
            sh2 = (seed & 2) != 0 ? 4 : 5;
        }
        var sh3 = (seed & 0x10) != 0 ? sh1 : sh2;

        seed1 = (byte)(seed1 >> sh1);
        seed2 = (byte)(seed2 >> sh2);
        seed3 = (byte)(seed3 >> sh1);
        seed4 = (byte)(seed4 >> sh2);
        seed5 = (byte)(seed5 >> sh1);
        seed6 = (byte)(seed6 >> sh2);
        seed7 = (byte)(seed7 >> sh1);
        seed8 = (byte)(seed8 >> sh2);
        seed9 = (byte)(seed9 >> sh3);
        seed10 = (byte)(seed10 >> sh3);
        seed11 = (byte)(seed11 >> sh3);
        seed12 = (byte)(seed12 >> sh3);

        // (the C++ adds unsigned values here; only the low 6 bits are kept, so the sums wrap the same way)
        var a = (int)((uint)(seed1 * x + seed2 * y + seed11 * z) + (rnum >> 14)) & 0x3F;
        var b = (int)((uint)(seed3 * x + seed4 * y + seed12 * z) + (rnum >> 10)) & 0x3F;
        var c = (int)((uint)(seed5 * x + seed6 * y + seed9 * z) + (rnum >> 6)) & 0x3F;
        var d = (int)((uint)(seed7 * x + seed8 * y + seed10 * z) + (rnum >> 2)) & 0x3F;

        if (partitionCount < 4)
            d = 0;
        if (partitionCount < 3)
            c = 0;

        if (a >= b && a >= c && a >= d)
            return 0;
        if (b >= c && b >= d)
            return 1;
        if (c >= d)
            return 2;
        return 3;
    }

    static bool IsHDRColorEndpointMode(uint cem) => cem is 2 or 3 or 7 or 11 or 14 or 15;

    static int SignExtend(int value, int nbits)
    {
        var signBit = 1 << (nbits - 1);
        return (value ^ signBit) - signBit;
    }

    static void DecodeHDREndpointMode7(uint v0, uint v1, uint v2, uint v3, out int r0, out int g0, out int b0, out int r1,
        out int g1, out int b1)
    {
        var modeval = ((v0 & 0xC0) >> 6) | ((v1 & 0x80) >> 5) | ((v2 & 0x80) >> 4);
        uint majcomp, mode;
        if ((modeval & 0xC) != 0xC)
        {
            majcomp = modeval >> 2;
            mode = modeval & 3;
        }
        else if (modeval != 0xF)
        {
            majcomp = modeval & 3;
            mode = 4;
        }
        else
        {
            majcomp = 0;
            mode = 5;
        }

        var red = (int)(v0 & 0x3f);
        var green = (int)(v1 & 0x1f);
        var blue = (int)(v2 & 0x1f);
        var scale = (int)(v3 & 0x1f);

        var x0 = (int)((v1 >> 6) & 1);
        var x1 = (int)((v1 >> 5) & 1);
        var x2 = (int)((v2 >> 6) & 1);
        var x3 = (int)((v2 >> 5) & 1);
        var x4 = (int)((v3 >> 7) & 1);
        var x5 = (int)((v3 >> 6) & 1);
        var x6 = (int)((v3 >> 5) & 1);

        var ohm = 1u << (int)mode;
        if ((ohm & 0x30) != 0) green |= x0 << 6;
        if ((ohm & 0x3A) != 0) green |= x1 << 5;
        if ((ohm & 0x30) != 0) blue |= x2 << 6;
        if ((ohm & 0x3A) != 0) blue |= x3 << 5;
        if ((ohm & 0x3D) != 0) scale |= x6 << 5;
        if ((ohm & 0x2D) != 0) scale |= x5 << 6;
        if ((ohm & 0x04) != 0) scale |= x4 << 7;
        if ((ohm & 0x3B) != 0) red |= x4 << 6;
        if ((ohm & 0x04) != 0) red |= x3 << 6;
        if ((ohm & 0x10) != 0) red |= x5 << 7;
        if ((ohm & 0x0F) != 0) red |= x2 << 7;
        if ((ohm & 0x05) != 0) red |= x1 << 8;
        if ((ohm & 0x0A) != 0) red |= x0 << 8;
        if ((ohm & 0x05) != 0) red |= x0 << 9;
        if ((ohm & 0x02) != 0) red |= x6 << 9;
        if ((ohm & 0x01) != 0) red |= x3 << 10;
        if ((ohm & 0x02) != 0) red |= x5 << 10;

        int[] shamts = { 1, 1, 2, 3, 4, 5 };
        var shamt = shamts[mode];
        red <<= shamt;
        green <<= shamt;
        blue <<= shamt;
        scale <<= shamt;

        if (mode != 5)
        {
            green = red - green;
            blue = red - blue;
        }

        if (majcomp == 1)
            (red, green) = (green, red);
        if (majcomp == 2)
            (red, blue) = (blue, red);

        r1 = Math.Clamp(red, 0, 0xFFF);
        g1 = Math.Clamp(green, 0, 0xFFF);
        b1 = Math.Clamp(blue, 0, 0xFFF);
        r0 = Math.Clamp(red - scale, 0, 0xFFF);
        g0 = Math.Clamp(green - scale, 0, 0xFFF);
        b0 = Math.Clamp(blue - scale, 0, 0xFFF);
    }

    static (int r0, int g0, int b0, int r1, int g1, int b1) DecodeHDREndpointMode11(uint v0, uint v1, uint v2, uint v3, uint v4, uint v5)
    {
        var majcomp = ((v4 & 0x80) >> 7) | ((v5 & 0x80) >> 6);
        if (majcomp == 3)
            return ((int)(v0 << 4), (int)(v2 << 4), (int)((v4 & 0x7f) << 5), (int)(v1 << 4), (int)(v3 << 4), (int)((v5 & 0x7f) << 5));

        var mode = ((v1 & 0x80) >> 7) | ((v2 & 0x80) >> 6) | ((v3 & 0x80) >> 5);
        var va = (int)(v0 | ((v1 & 0x40) << 2));
        var vb0 = (int)(v2 & 0x3f);
        var vb1 = (int)(v3 & 0x3f);
        var vc = (int)(v1 & 0x3f);
        var vd0 = (int)(v4 & 0x7f);
        var vd1 = (int)(v5 & 0x7f);

        int[] dbitstab = { 7, 6, 7, 6, 5, 6, 5, 6 };
        vd0 = SignExtend(vd0, dbitstab[mode]);
        vd1 = SignExtend(vd1, dbitstab[mode]);

        var x0 = (int)((v2 >> 6) & 1);
        var x1 = (int)((v3 >> 6) & 1);
        var x2 = (int)((v4 >> 6) & 1);
        var x3 = (int)((v5 >> 6) & 1);
        var x4 = (int)((v4 >> 5) & 1);
        var x5 = (int)((v5 >> 5) & 1);

        var ohm = 1u << (int)mode;
        if ((ohm & 0xA4) != 0) va |= x0 << 9;
        if ((ohm & 0x08) != 0) va |= x2 << 9;
        if ((ohm & 0x50) != 0) va |= x4 << 9;
        if ((ohm & 0x50) != 0) va |= x5 << 10;
        if ((ohm & 0xA0) != 0) va |= x1 << 10;
        if ((ohm & 0xC0) != 0) va |= x2 << 11;
        if ((ohm & 0x04) != 0) vc |= x1 << 6;
        if ((ohm & 0xE8) != 0) vc |= x3 << 6;
        if ((ohm & 0x20) != 0) vc |= x2 << 7;
        if ((ohm & 0x5B) != 0) vb0 |= x0 << 6;
        if ((ohm & 0x5B) != 0) vb1 |= x1 << 6;
        if ((ohm & 0x12) != 0) vb0 |= x2 << 7;
        if ((ohm & 0x12) != 0) vb1 |= x3 << 7;

        var shamt = ((int)mode >> 1) ^ 3;
        va <<= shamt;
        vb0 <<= shamt;
        vb1 <<= shamt;
        vc <<= shamt;
        vd0 <<= shamt;
        vd1 <<= shamt;

        var r1 = Math.Clamp(va, 0, 0xFFF);
        var g1 = Math.Clamp(va - vb0, 0, 0xFFF);
        var b1 = Math.Clamp(va - vb1, 0, 0xFFF);
        var r0 = Math.Clamp(va - vc, 0, 0xFFF);
        var g0 = Math.Clamp(va - vb0 - vc - vd0, 0, 0xFFF);
        var b0 = Math.Clamp(va - vb1 - vc - vd1, 0, 0xFFF);

        if (majcomp == 1)
        {
            (r0, g0) = (g0, r0);
            (r1, g1) = (g1, r1);
        }
        else if (majcomp == 2)
        {
            (r0, b0) = (b0, r0);
            (r1, b1) = (b1, r1);
        }
        return (r0, g0, b0, r1, g1, b1);
    }

    /// <summary>The two endpoints of one partition (section C.2.14). colorValues advances past the values it uses.</summary>
    static void ComputeEndpoints(out Pixel ep1, out Pixel ep2, ReadOnlySpan<uint> colorValues, ref int at, uint colorEndpointMode)
    {
        var v = colorValues[at..];

        switch (colorEndpointMode)
        {
            case 0:
                ep1 = new Pixel(0xFF, (int)v[0], (int)v[0], (int)v[0]);
                ep2 = new Pixel(0xFF, (int)v[1], (int)v[1], (int)v[1]);
                at += 2;
                break;

            case 1:
            {
                var L0 = (v[0] >> 2) | (v[1] & 0xC0);
                var L1 = Math.Min(L0 + (v[1] & 0x3F), 0xFFu);
                ep1 = new Pixel(0xFF, (int)L0, (int)L0, (int)L0);
                ep2 = new Pixel(0xFF, (int)L1, (int)L1, (int)L1);
                at += 2;
                break;
            }

            case 4:
                ep1 = new Pixel((int)v[2], (int)v[0], (int)v[0], (int)v[0]);
                ep2 = new Pixel((int)v[3], (int)v[1], (int)v[1], (int)v[1]);
                at += 4;
                break;

            case 5:
            {
                int v0 = (int)v[0], v1 = (int)v[1], v2 = (int)v[2], v3 = (int)v[3];
                BitTransferSigned(ref v1, ref v0);
                BitTransferSigned(ref v3, ref v2);
                ep1 = new Pixel(v2, v0, v0, v0);
                ep2 = new Pixel(v2 + v3, v0 + v1, v0 + v1, v0 + v1);
                ep1.ClampByte();
                ep2.ClampByte();
                at += 4;
                break;
            }

            case 6:
                ep1 = new Pixel(0xFF, (int)(v[0] * v[3] >> 8), (int)(v[1] * v[3] >> 8), (int)(v[2] * v[3] >> 8));
                ep2 = new Pixel(0xFF, (int)v[0], (int)v[1], (int)v[2]);
                at += 4;
                break;

            case 8:
                if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4])
                {
                    ep1 = new Pixel(0xFF, (int)v[0], (int)v[2], (int)v[4]);
                    ep2 = new Pixel(0xFF, (int)v[1], (int)v[3], (int)v[5]);
                }
                else
                {
                    ep1 = BlueContract(0xFF, (int)v[1], (int)v[3], (int)v[5]);
                    ep2 = BlueContract(0xFF, (int)v[0], (int)v[2], (int)v[4]);
                }
                at += 6;
                break;

            case 9:
            {
                int v0 = (int)v[0], v1 = (int)v[1], v2 = (int)v[2], v3 = (int)v[3], v4 = (int)v[4], v5 = (int)v[5];
                BitTransferSigned(ref v1, ref v0);
                BitTransferSigned(ref v3, ref v2);
                BitTransferSigned(ref v5, ref v4);
                if (v1 + v3 + v5 >= 0)
                {
                    ep1 = new Pixel(0xFF, v0, v2, v4);
                    ep2 = new Pixel(0xFF, v0 + v1, v2 + v3, v4 + v5);
                }
                else
                {
                    ep1 = BlueContract(0xFF, v0 + v1, v2 + v3, v4 + v5);
                    ep2 = BlueContract(0xFF, v0, v2, v4);
                }
                ep1.ClampByte();
                ep2.ClampByte();
                at += 6;
                break;
            }

            case 10:
                ep1 = new Pixel((int)v[4], (int)(v[0] * v[3] >> 8), (int)(v[1] * v[3] >> 8), (int)(v[2] * v[3] >> 8));
                ep2 = new Pixel((int)v[5], (int)v[0], (int)v[1], (int)v[2]);
                at += 6;
                break;

            case 12:
                if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4])
                {
                    ep1 = new Pixel((int)v[6], (int)v[0], (int)v[2], (int)v[4]);
                    ep2 = new Pixel((int)v[7], (int)v[1], (int)v[3], (int)v[5]);
                }
                else
                {
                    ep1 = BlueContract((int)v[7], (int)v[1], (int)v[3], (int)v[5]);
                    ep2 = BlueContract((int)v[6], (int)v[0], (int)v[2], (int)v[4]);
                }
                at += 8;
                break;

            case 13:
            {
                int v0 = (int)v[0], v1 = (int)v[1], v2 = (int)v[2], v3 = (int)v[3], v4 = (int)v[4], v5 = (int)v[5], v6 = (int)v[6], v7 = (int)v[7];
                BitTransferSigned(ref v1, ref v0);
                BitTransferSigned(ref v3, ref v2);
                BitTransferSigned(ref v5, ref v4);
                BitTransferSigned(ref v7, ref v6);
                if (v1 + v3 + v5 >= 0)
                {
                    ep1 = new Pixel(v6, v0, v2, v4);
                    ep2 = new Pixel(v7 + v6, v0 + v1, v2 + v3, v4 + v5);
                }
                else
                {
                    ep1 = BlueContract(v6 + v7, v0 + v1, v2 + v3, v4 + v5);
                    ep2 = BlueContract(v6, v0, v2, v4);
                }
                ep1.ClampByte();
                ep2.ClampByte();
                at += 8;
                break;
            }

            case 2:
            {
                uint y0, y1;
                if (v[1] >= v[0])
                {
                    y0 = v[0] << 4;
                    y1 = v[1] << 4;
                }
                else
                {
                    y0 = (v[1] << 4) + 8;
                    y1 = (v[0] << 4) - 8;
                }
                ep1 = new Pixel(0x780, (int)y0, (int)y0, (int)y0);
                ep2 = new Pixel(0x780, (int)y1, (int)y1, (int)y1);
                at += 2;
                break;
            }

            case 3:
            {
                uint y0, d;
                if ((v[0] & 0x80) != 0)
                {
                    y0 = ((v[1] & 0xE0) << 4) | ((v[0] & 0x7F) << 2);
                    d = (v[1] & 0x1F) << 2;
                }
                else
                {
                    y0 = ((v[1] & 0xF0) << 4) | ((v[0] & 0x7F) << 1);
                    d = (v[1] & 0x0F) << 1;
                }
                var y1 = Math.Min(y0 + d, 0xFFFu);
                ep1 = new Pixel(0x780, (int)y0, (int)y0, (int)y0);
                ep2 = new Pixel(0x780, (int)y1, (int)y1, (int)y1);
                at += 2;
                break;
            }

            case 7:
            {
                DecodeHDREndpointMode7(v[0], v[1], v[2], v[3], out var r0, out var g0, out var b0, out var r1, out var g1, out var b1);
                ep1 = new Pixel(0x780, r0, g0, b0);
                ep2 = new Pixel(0x780, r1, g1, b1);
                at += 4;
                break;
            }

            case 11:
            {
                var rgb = DecodeHDREndpointMode11(v[0], v[1], v[2], v[3], v[4], v[5]);
                ep1 = new Pixel(0x780, rgb.r0, rgb.g0, rgb.b0);
                ep2 = new Pixel(0x780, rgb.r1, rgb.g1, rgb.b1);
                at += 6;
                break;
            }

            case 14:
            {
                var rgb = DecodeHDREndpointMode11(v[0], v[1], v[2], v[3], v[4], v[5]);
                ep1 = new Pixel((int)v[6], rgb.r0, rgb.g0, rgb.b0);
                ep2 = new Pixel((int)v[7], rgb.r1, rgb.g1, rgb.b1);
                at += 8;
                break;
            }

            case 15:
            {
                var rgb = DecodeHDREndpointMode11(v[0], v[1], v[2], v[3], v[4], v[5]);
                var mode = (int)(((v[6] >> 7) & 1) | ((v[7] >> 6) & 2));
                var a6 = (int)(v[6] & 0x7F);
                var a7 = (int)(v[7] & 0x7F);
                int alpha0, alpha1;
                if (mode == 3)
                {
                    alpha0 = a6 << 5;
                    alpha1 = a7 << 5;
                }
                else
                {
                    a6 |= (a7 << (mode + 1)) & 0x780;
                    a7 &= 0x3F >> mode;
                    a7 ^= 0x20 >> mode;
                    a7 -= 0x20 >> mode;
                    a6 <<= 4 - mode;
                    a7 <<= 4 - mode;
                    a7 += a6;
                    alpha0 = a6;
                    alpha1 = Math.Clamp(a7, 0, 0xFFF);
                }
                ep1 = new Pixel(alpha0, rgb.r0, rgb.g0, rgb.b0);
                ep2 = new Pixel(alpha1, rgb.r1, rgb.g1, rgb.b1);
                at += 8;
                break;
            }

            default:
                ep1 = default;
                ep2 = default;
                break;
        }
    }

    static void FillVoidExtentLDR(ref InputBitStream strm, Span<uint> outBuf, int blockWidth, int blockHeight)
    {
        // (the extent itself does not matter here: its bits are read past)
        for (var i = 0; i < 4; i++)
            strm.ReadBits(13);
        // (the RGBA components, back to 0..255)
        var r = (ushort)strm.ReadBits(16);
        var g = (ushort)strm.ReadBits(16);
        var b = (ushort)strm.ReadBits(16);
        var a = (ushort)strm.ReadBits(16);
        var rgba = (uint)(r >> 8) | (uint)(g & 0xFF00) | ((uint)b & 0xFF00) << 8 | ((uint)a & 0xFF00) << 16;
        outBuf[..(blockWidth * blockHeight)].Fill(rgba);
    }

    static float HalfToFloat(ushort h) => (float)BitConverter.UInt16BitsToHalf(h);

    static ushort HalfToClampedByte(ushort halfBits)
    {
        var value = HalfToFloat(halfBits);
        var clamped = Math.Clamp(value, 0.0f, 1.0f);
        return (ushort)(clamped * 255.0f + 0.5f);
    }

    static void FillError(Span<uint> outBuf, int blockWidth, int blockHeight) => outBuf[..(blockWidth * blockHeight)].Clear();

    static uint ReverseByte(byte b) => (uint)((((ulong)b * 0x80200802UL) & 0x0884422110UL) * 0x0101010101UL >> 32) & 0xFF;

    /// <summary>Per-thread working memory of the block decoder.</summary>
    sealed class Scratch
    {
        public readonly EncodedVector Colors = new(), Weights = new();
        public readonly uint[][] WeightsOut = { new uint[144], new uint[144] };
        public readonly uint[][] Unquantized = { new uint[144], new uint[144] };
        public readonly uint[] Block = new uint[144];
    }

    static void DecompressBlock(ReadOnlySpan<byte> inBuf, int blockWidth, int blockHeight, Span<uint> outBuf, Scratch scratch)
    {
        var strm = new InputBitStream(inBuf);
        var weightParams = DecodeBlockInfo(ref strm);

        if (weightParams.Error)
        {
            FillError(outBuf, blockWidth, blockHeight);
            return;
        }
        if (weightParams.VoidExtentLDR)
        {
            FillVoidExtentLDR(ref strm, outBuf, blockWidth, blockHeight);
            return;
        }
        if (weightParams.VoidExtentHDR || weightParams.Width > blockWidth || weightParams.Height > blockHeight)
        {
            FillError(outBuf, blockWidth, blockHeight);
            return;
        }

        // (the number of partitions)
        var nPartitions = (int)strm.ReadBits(2) + 1;
        if (nPartitions == 4 && weightParams.DualPlane)
        {
            FillError(outBuf, blockWidth, blockHeight);
            return;
        }

        // (partitions, the partition index and the color endpoint modes)
        uint partitionIndex;
        Span<uint> colorEndpointMode = stackalloc uint[4];
        Span<byte> colorEndpointData = stackalloc byte[16];
        colorEndpointData.Clear();
        var colorEndpointStream = new OutputBitStream(colorEndpointData, 16 * 8);

        uint baseCEM = 0;
        if (nPartitions == 1)
        {
            colorEndpointMode[0] = strm.ReadBits(4);
            partitionIndex = 0;
        }
        else
        {
            partitionIndex = strm.ReadBits(10);
            baseCEM = strm.ReadBits(6);
        }
        var baseMode = baseCEM & 3;

        // (what is left is color endpoint data...)
        var nWeightBits = weightParams.GetPackedBitSize();
        var remainingBits = 128 - nWeightBits - strm.BitsRead;

        // (...less the extra CEM bits before the weights...)
        var extraCEMbits = 0;
        if (baseMode != 0)
        {
            extraCEMbits = nPartitions switch { 2 => 2, 3 => 5, 4 => 8, _ => 0 };
        }
        remainingBits -= extraCEMbits;

        // (...and the plane selector of a dual plane block)
        var planeSelectorBits = weightParams.DualPlane ? 2 : 0;
        remainingBits -= planeSelectorBits;

        // (the color data)
        var colorDataBits = remainingBits;
        while (remainingBits > 0)
        {
            var nb = Math.Min(remainingBits, 8);
            var b = strm.ReadBits(nb);
            colorEndpointStream.WriteBits(b, nb);
            remainingBits -= 8;
        }

        // (the plane selector)
        var planeIdx = strm.ReadBits(planeSelectorBits);

        // (the rest of the CEM)
        if (baseMode != 0)
        {
            var extraCEM = strm.ReadBits(extraCEMbits);
            var CEM = (extraCEM << 6) | baseCEM;
            CEM >>= 2;
            Span<bool> C = stackalloc bool[4];
            for (var i = 0; i < nPartitions; i++)
            {
                C[i] = (CEM & 1) != 0;
                CEM >>= 1;
            }
            Span<byte> M = stackalloc byte[4];
            for (var i = 0; i < nPartitions; i++)
            {
                M[i] = (byte)(CEM & 3);
                CEM >>= 2;
            }
            for (var i = 0; i < nPartitions; i++)
            {
                colorEndpointMode[i] = baseMode;
                if (!C[i])
                    colorEndpointMode[i] -= 1;
                colorEndpointMode[i] <<= 2;
                colorEndpointMode[i] |= M[i];
            }
        }
        else if (nPartitions > 1)
        {
            var CEM = baseCEM >> 2;
            for (var i = 0; i < nPartitions; i++)
                colorEndpointMode[i] = CEM;
        }

        // (the color values, then the endpoints)
        Span<uint> colorValues = stackalloc uint[32];
        if (colorDataBits < 0)
        {
            FillError(outBuf, blockWidth, blockHeight);
            return;
        }
        DecodeColorValues(colorValues, colorEndpointData, colorEndpointMode, nPartitions, colorDataBits, scratch.Colors);

        Span<Pixel> endpoints = stackalloc Pixel[8];
        var at = 0;
        for (var i = 0; i < nPartitions; i++)
        {
            if (at + 8 > colorValues.Length && colorEndpointMode[i] >= 12)
            {
                FillError(outBuf, blockWidth, blockHeight);
                return;
            }
            ComputeEndpoints(out endpoints[2 * i], out endpoints[2 * i + 1], colorValues, ref at, colorEndpointMode[i]);
        }

        // (the texel weights: the block's bits reversed, the weights read from what were its top bits)
        Span<byte> texelWeightData = stackalloc byte[16];
        inBuf[..16].CopyTo(texelWeightData);
        for (var i = 0; i < 8; i++)
        {
            var a = (byte)ReverseByte(texelWeightData[i]);
            var b = (byte)ReverseByte(texelWeightData[15 - i]);
            texelWeightData[i] = b;
            texelWeightData[15 - i] = a;
        }

        // (the bits above the weights cleared)
        var packed = weightParams.GetPackedBitSize();
        var clearByteStart = (packed >> 3) + 1;
        if (clearByteStart > 0 && clearByteStart <= 16)
        {
            texelWeightData[clearByteStart - 1] &= (byte)((1 << (packed % 8)) - 1);
            texelWeightData[clearByteStart..].Clear();
        }

        scratch.Weights.Clear();
        var weightStream = new InputBitStream(texelWeightData);
        DecodeIntegerSequence(scratch.Weights, ref weightStream, weightParams.MaxWeight, weightParams.GetNumWeightValues());

        var weights = scratch.WeightsOut;
        UnquantizeTexelWeights(weights, scratch.Weights, weightParams, blockWidth, blockHeight, scratch.Unquantized);

        // (endpoints and weights: the texels)
        var small = blockHeight * blockWidth < 32;
        for (var j = 0; j < blockHeight; j++)
            for (var i = 0; i < blockWidth; i++)
            {
                var partition = SelectPartition((int)partitionIndex, i, j, 0, nPartitions, small);
                var p = new Pixel();
                var cem = colorEndpointMode[partition];
                for (var c = 0; c < 4; c++)
                {
                    var C0 = (uint)endpoints[2 * partition].Component(c);
                    var C1 = (uint)endpoints[2 * partition + 1].Component(c);
                    var plane = weightParams.DualPlane && ((planeIdx + 1) & 3) == c ? 1 : 0;
                    var weight = weights[plane][j * blockWidth + i];

                    var isHdr = IsHDRColorEndpointMode(cem) && !(cem == 14 && c == 0);
                    if (isHdr)
                    {
                        C0 <<= 4;
                        C1 <<= 4;
                        var C = (C0 * (64 - weight) + C1 * weight + 32) / 64;
                        var E = (C & 0xF800) >> 11;
                        var M = C & 0x7FF;
                        uint Mt;
                        if (M < 512)
                            Mt = 3 * M;
                        else if (M >= 1536)
                            Mt = 5 * M - 2048;
                        else
                            Mt = 4 * M - 512;
                        var Cf = (E << 10) + (Mt >> 3);
                        var halfBits = Cf >= 0x7C00 ? (ushort)0x7BFF : (ushort)Cf;
                        p.SetComponent(c, HalfToClampedByte(halfBits));
                    }
                    else
                    {
                        C0 = ReplicateByteTo16Table[C0 & 0xFF];
                        C1 = ReplicateByteTo16Table[C1 & 0xFF];
                        var C = (C0 * (64 - weight) + C1 * weight + 32) / 64;
                        if (C == 65535)
                            p.SetComponent(c, 255);
                        else
                            p.SetComponent(c, (ushort)(255.0 * (C / 65536.0) + 0.5));
                    }
                }
                outBuf[j * blockWidth + i] = p.Pack();
            }
    }

    /// <summary>An ASTC image (blocks in rows, 16 bytes each) decoded to RGBA8, width * height * 4 bytes. Rows of blocks are
    /// decoded on several threads.</summary>
    public static byte[] Decompress(ReadOnlyMemory<byte> data, int width, int height, int blockWidth, int blockHeight)
    {
        if (blockWidth < 4 || blockWidth > 12 || blockHeight < 4 || blockHeight > 12)
            throw new NotSupportedException("ASTC block " + blockWidth + "x" + blockHeight);
        var rows = (height + blockHeight - 1) / blockHeight;
        var cols = (width + blockWidth - 1) / blockWidth;
        if (data.Length < rows * cols * 16)
            throw new InvalidDataException("ASTC data is shorter than the image");
        var output = new byte[width * height * 4];
        Parallel.For(0, rows, () => new Scratch(), (yIndex, _, scratch) =>
        {
            var src = data.Span;
            var y = yIndex * blockHeight;
            var block = scratch.Block;
            for (var xIndex = 0; xIndex < cols; xIndex++)
            {
                var blockIndex = yIndex * cols + xIndex;
                var x = xIndex * blockWidth;
                DecompressBlock(src.Slice(blockIndex * 16, 16), blockWidth, blockHeight, block, scratch);
                var decompWidth = Math.Min(blockWidth, width - x);
                var decompHeight = Math.Min(blockHeight, height - y);
                for (var h = 0; h < decompHeight; h++)
                {
                    var into = output.AsSpan(((y + h) * width + x) * 4, decompWidth * 4);
                    System.Runtime.InteropServices.MemoryMarshal.AsBytes(block.AsSpan(h * blockWidth, decompWidth)).CopyTo(into);
                }
            }
            return scratch;
        }, _ => { });
        return output;
    }
}
