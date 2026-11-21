// What keeps a run of the launcher from damaging an install (old setup/Guard.cs). Nothing here needs a window.
namespace Triforce.Logic;

/// <summary>Room on a disk, asked before it is needed. A download or an unpacking that ran out of room stopped somewhere in the
/// middle with Windows' own words for it and left half of itself behind; asked first, the player is told how much it takes and
/// how much there is, before anything is written.</summary>
public static class Space
{
    /// <summary>What a disk keeps free beyond what is about to be written to it.</summary>
    public const long Margin = 32L << 20;

    /// <summary>Null when the free bytes are room for need more of them and the margin, or are not known; else the sentence that
    /// says there is no room for what, how much it takes and how much is free (what it takes rounded up and what is free rounded
    /// down, so the two numbers never read the same).</summary>
    public static string? Problem(string what, long need, long? free, string disk)
    {
        need = Math.Max(0, need) + Margin;
        if (free is not { } f || f >= need) return null;
        return "There is no room for " + what + " on " + disk + ": it takes " + ((need + (1 << 20) - 1) >> 20) + " MB there, and " + (Math.Max(0, f) >> 20) +
               " MB are free. Make room and try again.";
    }
}

/// <summary>There is no room for what is about to be written (Space.Problem's sentence), said before any of it is.</summary>
public sealed class NoRoomException : IOException
{
    public NoRoomException(string message) : base(message) { }
}

/// <summary>A download stopped before its end (the connection was cut, went silent or closed early): its part is kept, and the
/// next try goes on from it.</summary>
public sealed class CutOffException : IOException
{
    public CutOffException(string message, Exception? inner = null) : base(message, inner) { }
}

/// <summary>The launcher an update brought could not start, and what was done about it.</summary>
public sealed class NotStartedException : Exception
{
    public NotStartedException(string message) : base(message) { }
}

/// <summary>The release server has no release (404, an empty list, an empty tag).</summary>
public sealed class NoReleaseException : InvalidOperationException
{
    public NoReleaseException() : base("There is no Triforce release yet.") { }
}
