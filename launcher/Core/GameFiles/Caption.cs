// A save slot's picture: each slot folder of the game's saves (slot_00 .. slot_05) has a caption.sav that holds, among
// the slot's details, a JPEG of the place the player saved (research/scratch/t09_caption.py). The JPEG is taken as the
// bytes from its first start marker (FF D8 FF) to its last end marker (FF D9), which needs no knowledge of the .sav layout.
namespace Triforce.Core.GameFiles;

public static class Caption
{
    /// <summary>The JPEG inside a caption.sav's bytes, or null when there is none.</summary>
    public static byte[]? Jpeg(ReadOnlySpan<byte> captionSav)
    {
        ReadOnlySpan<byte> start = stackalloc byte[] { 0xFF, 0xD8, 0xFF };
        ReadOnlySpan<byte> end = stackalloc byte[] { 0xFF, 0xD9 };
        var from = captionSav.IndexOf(start);
        if (from < 0)
            return null;
        var to = captionSav.LastIndexOf(end);
        if (to < from)
            return null;
        return captionSav[from..(to + 2)].ToArray();
    }

    /// <summary>The JPEG of a caption.sav file (read only, shared with the game), or null when there is none.</summary>
    public static byte[]? JpegFromFile(string captionSavPath)
    {
        using var f = new FileStream(captionSavPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        var data = new byte[f.Length];
        f.ReadExactly(data);
        return Jpeg(data);
    }
}
