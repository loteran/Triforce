using System;
using System.IO;
using System.Threading;

namespace HookshotBuilder;

/// (Triforce, handoff §5) One Hookshot Builder at a time for its build tools. Two of them on one tools folder ran make_game.py on
/// the same work folder, and make_game.py stops a build it finds running there: the first one's build ended without a reason.
/// The first builder holds a named mutex for as long as it runs (Windows lets go of it when the process ends, however it ends);
/// a second one for the same tools says so and ends. The name comes from the tools folder (Tools.Root), so builders of other
/// folders, Triforce Setup (its lock is named after its own folder) and the game (after its game folder) never meet it.
/// HOOKSHOT_SINGLE_INSTANCE=0: no lock (as before). Plain logic: tests/test_builder_instance.py compiles this file alone
static class Instance
{
    static Mutex? held;

    /// The lock's name for a tools folder: one for each spelling of it (case, '/' or '\', a separator at its end), a 64-bit FNV-1a
    /// of that spelling. Local\: this user's session
    public static string Name(string root)
    {
        try { root = Path.GetFullPath(root); } catch (Exception) { }
        var text = root.Replace('/', '\\').TrimEnd('\\').ToUpperInvariant();
        var h = 14695981039346656037UL;   // (a name, not a secret)
        foreach (var c in text) { h ^= c; h *= 1099511628211UL; }
        return @"Local\HookshotBuilder-" + h.ToString("x16");
    }

    /// True when this process now holds the tools folder (or did already; it waited up to waitMs for a builder that was ending),
    /// false when another builder does. Where no lock can be made at all nobody is refused
    public static bool Take(string root, int waitMs = 5000)
    {
        if (held != null) return true;
        if (Environment.GetEnvironmentVariable("HOOKSHOT_SINGLE_INSTANCE") == "0") return true;
        Mutex m;
        try { m = new Mutex(false, Name(root)); }
        catch (UnauthorizedAccessException) { return false; }   // (it is there, and belongs to a builder run as administrator)
        catch (Exception) { return true; }
        try
        {
            if (!m.WaitOne(waitMs)) { m.Dispose(); return false; }
        }
        catch (AbandonedMutexException) { }   // (its holder ended without letting go: Windows hands it on, and it is ours)
        catch (Exception) { m.Dispose(); return true; }
        held = m;
        return true;
    }

    /// What a second builder says
    public static string Refusal(string root) =>
        "Hookshot Builder is already running for these build tools:\n\n" + root +
        "\n\nUse the window that is open, or close it and start the builder again. (Two builders on one tools folder would build " +
        "into the same work folder and stop each other's build.)";
}
