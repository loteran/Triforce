namespace Triforce.Logic;

/// <summary>(Triforce) The save's lock between the game and the launcher (old Guard.cs SaveLock). The game holds
/// &lt;user&gt;\save.lock open without sharing from its first save mount to its end (host/save_tx.h save_tx_lock); NX2navi holds
/// &lt;profile&gt;\instance.lock the same way. Taken by the launcher across the whole copy, the lock says for certain whether the game
/// runs, and a game started meanwhile waits for it before it mounts its save.
///
/// Linux: .NET takes FileShare.None as an advisory flock there, and the game takes none (research/02 section 11: "No Linux guard
/// exists today"), so on Linux only the process check (Saves.RunningGame) guards a copy.</summary>
public static class SaveLock
{
    public const string Running = "The game is running: quit it first. It keeps its save open while it runs, and would write over the copy.";

    /// <summary>The lock of a save folder &lt;user&gt;\save\&lt;uid&gt; (or a commit waiting in &lt;user&gt;\save-tx\&lt;uid&gt;\ready-&lt;n&gt;):
    /// &lt;user&gt;\save.lock, or instance.lock in NX2navi's profile folder; null for another kind of save folder (an emulator's).</summary>
    public static string? PathOf(string save)
    {
        string dir;
        try { dir = Path.GetFullPath(save).TrimEnd('\\', '/'); }
        catch (Exception) { return null; }
        var parent = Path.GetDirectoryName(dir);
        if (parent != null && Path.GetDirectoryName(parent) is { } tx && string.Equals(Path.GetFileName(tx), "save-tx", StringComparison.OrdinalIgnoreCase))
            parent = Path.Combine(Path.GetDirectoryName(tx) ?? tx, "save");
        if (parent == null || !string.Equals(Path.GetFileName(parent), "save", StringComparison.OrdinalIgnoreCase)) return null;
        var user = Path.GetDirectoryName(parent);
        if (string.IsNullOrEmpty(user)) return null;
        return Path.Combine(user, string.Equals(Path.GetFileName(user), "profile", StringComparison.OrdinalIgnoreCase) ? "instance.lock" : "save.lock");
    }

    /// <summary>The lock of a save folder, held until it is disposed (an open handle that shares nothing). Null when the folder has
    /// none or it cannot be made there (nothing is locked: the copy goes on as before). InvalidOperationException (Running) when a
    /// game holds it.</summary>
    public static FileStream? Take(string save)
    {
        if (PathOf(save) is not { } path) return null;
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            return new FileStream(path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        }
        catch (IOException e) when ((e.HResult & 0xFFFF) == 32 || (e.HResult & 0xFFFF) == 33 || !OperatingSystem.IsWindows() && (e.HResult & 0xFFFF) == 11)
        {
            // (ERROR_SHARING_VIOLATION, ERROR_LOCK_VIOLATION; EWOULDBLOCK from flock)
            throw new InvalidOperationException(Running);
        }
        catch (Exception) { return null; }
    }
}
