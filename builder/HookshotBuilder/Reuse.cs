using System;
using System.IO;
using System.Linq;
using System.Threading;

namespace HookshotBuilder;

/// (Triforce) What the setup (builder/setup/setup.c) lends from the installed version to the one it puts together, and the lock
/// every setup holds while it does. An update moves the folders of parts that did not change (Python, the compiler...) from
/// versions\<installed> into versions\<tag>.part and writes each down first in its reused.txt (the installed version's tag,
/// then a folder a line); a cut-off setup leaves them there. Free gives them back (UndoEveryReuse, setup.c undo_every_reuse,
/// the same rules) and never deletes a <tag>.part that still holds them (Lent): deleted, the installed version lost them for
/// good. Plain logic: tests/test_builder_setup_lock.py compiles this file alone
static class Reuse
{
    public const string Note = "reused.txt";

    /// True when folder is an unfinished version (a versions\*.part) that still holds folders of another version
    public static bool Lent(string folder) =>
        Path.GetFileName(folder.TrimEnd('\\', '/')).EndsWith(".part", StringComparison.OrdinalIgnoreCase) &&
        File.Exists(Path.Combine(folder, Note));

    /// The setup's lock for a setup folder (setup.c setup_lock_name and setup_parts.lock_name, the same): a 64-bit FNV-1a of the
    /// folder's full path with '\' between its parts, none at its end, a to z in upper case. Local\: this user's session
    public static string LockName(string setupBase)
    {
        try { setupBase = Path.GetFullPath(setupBase); } catch (Exception) { }
        var text = setupBase.Replace('/', '\\').TrimEnd('\\');
        var h = 14695981039346656037UL;   // (a name, not a secret)
        foreach (var c in text) { h ^= c >= 'a' && c <= 'z' ? (uint)(c - 32) : c; h *= 1099511628211UL; }
        return @"Local\HookshotBuilder-Setup-" + h.ToString("x16");
    }

    /// The setup folder held by this thread until Dispose (one that holds nothing where no lock can be made at all), or null
    /// when a setup held it for waitMs: one is installing now
    public static Held? TakeLock(string setupBase, int waitMs = 0)
    {
        Mutex m;
        try { m = new Mutex(false, LockName(setupBase)); }
        catch (UnauthorizedAccessException) { return null; }   // (it is there, and belongs to a setup run as administrator)
        catch (Exception) { return new Held(null); }
        try
        {
            if (!m.WaitOne(waitMs)) { m.Dispose(); return null; }
        }
        catch (AbandonedMutexException) { }   // (its holder ended without letting go: Windows hands it on, and it is ours)
        catch (Exception) { m.Dispose(); return new Held(null); }
        return new Held(m);
    }

    public sealed class Held : IDisposable
    {
        Mutex? m;
        internal Held(Mutex? m) => this.m = m;

        public void Dispose()
        {
            if (m == null) return;
            try { m.ReleaseMutex(); } catch (Exception) { }
            m.Dispose();
            m = null;
        }
    }

    /// The folders a setup moved from the installed version into install (a <tag>.part), moved back: each only where that version
    /// has none; the note goes once all went back. True: nothing is left to move back
    public static bool Undo(string versions, string install)
    {
        var note = Path.Combine(install, Note);
        string[] lines;
        try { lines = File.ReadAllLines(note).Where(l => l.Trim().Length > 0).ToArray(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return !File.Exists(note); }
        if (lines.Length == 0 || !NameOk(lines[0], false))   // (no version named: nothing was moved yet)
        {
            try { File.Delete(note); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            return true;
        }
        var ok = true;
        foreach (var rel in lines.Skip(1))
        {
            if (!NameOk(rel, true)) continue;
            var from = Path.Combine(install, rel.Replace('/', '\\'));
            var back = Path.Combine(versions, lines[0], rel.Replace('/', '\\'));
            if (!(Directory.Exists(from) || File.Exists(from)) || Directory.Exists(back) || File.Exists(back)) continue;
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(back)!);
                if (Directory.Exists(from)) Directory.Move(from, back);
                else File.Move(from, back);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { ok = false; }
        }
        if (ok)
            try { File.Delete(note); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return ok;
    }

    /// Undo for every versions\*.part with a note; the first whose folders could not all go back (each is tried), null when none
    public static string? UndoEveryReuse(string versions)
    {
        string? left = null;
        string[] dirs;
        try { dirs = Directory.GetDirectories(versions); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
        foreach (var d in dirs.OrderBy(d => d, StringComparer.Ordinal))
            if (Lent(d) && !Undo(versions, d)) left ??= d;
        return left;
    }

    /// A name from the note (setup.c list_name_ok, the same): letters, digits and . _ + - only (with path, '/' or '\' between
    /// its parts), no "." or ".." part, no drive, no root
    static bool NameOk(string s, bool path)
    {
        if (s.Length == 0 || s[0] == '/' || s[0] == '\\') return false;
        foreach (var c in s)
            if (!(c is >= 'a' and <= 'z' or >= 'A' and <= 'Z' or >= '0' and <= '9' or '.' or '_' or '+' or '-' ||
                  (path && (c == '/' || c == '\\'))))
                return false;
        foreach (var part in path ? s.Split('/', '\\') : new[] { s })
            if (part.Length == 0 || part == "." || part == "..") return false;
        return true;
    }
}
