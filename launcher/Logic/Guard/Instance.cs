using System.Collections.Concurrent;
using System.Diagnostics;
using System.Net.Sockets;
using System.Runtime.InteropServices;

namespace Triforce.Logic;

/// <summary>One launcher at a time for an install (old Guard.cs Instance). Two of them on one folder each downloaded the tools into
/// the same files, unpacked them over each other and built into the same game folder. The first one holds a named mutex for as
/// long as it runs; Windows lets go of it when the process ends, however it ends. A second start for the same folder asks the
/// first to come to the front (a named event the first one waits on) and ends.
///
/// The names are the old Setup's exactly (research/02 2.6), so an old Setup started from a stale download defers to the new
/// launcher and the other way round: Local\TriforceSetup-&lt;FNV-1a 64 of the folder (and "|" test root), 16 hex&gt;-&lt;the folder's last
/// 60 characters, each one that is not a letter or digit as -&gt;; the event "&lt;name&gt;-show" brings the holder forward, and
/// "&lt;name&gt;-up" is the handover's sign (Handover).
///
/// A mutex belongs to the thread that took it: here one thread of its own takes it, holds it and lets go of it, so the
/// launcher's async code may Release from any thread (the old Setup's Release on another thread only closed the handle).
/// Linux: a lock file (flock through FileShare.None) in $XDG_RUNTIME_DIR, and a unix socket for "come forward".</summary>
public sealed class InstanceLock : IDisposable
{
    /// <summary>One spelling for a folder, however it was typed.</summary>
    static string Same(string path)
    {
        try { path = Path.GetFullPath(path); } catch (Exception) { }
        return path.TrimEnd('\\', '/').ToUpperInvariant();
    }

    /// <summary>FNV-1a 64 over the UTF-16 code units of a text (offset 14695981039346656037, prime 1099511628211).</summary>
    public static ulong Fnv1a64(string text)
    {
        var h = 14695981039346656037UL;
        foreach (var c in text) { h ^= c; h *= 1099511628211UL; }
        return h;
    }

    /// <summary>The lock's name for a folder: a number made from the whole path (and the test folder's), then the end of the path
    /// for whoever looks at it. (Local\: one user's session. An install is one user's, HKCU and %LOCALAPPDATA%)</summary>
    public static string Name(string folder, string? testRoot)
    {
        var text = Same(folder) + (testRoot != null ? "|" + Same(testRoot) : "");
        var h = Fnv1a64(text);
        var tail = new string(Same(folder).Select(c => char.IsLetterOrDigit(c) ? c : '-').ToArray());
        return @"Local\TriforceSetup-" + h.ToString("x16") + "-" + (tail.Length > 60 ? tail.Substring(tail.Length - 60) : tail);
    }

    /// <summary>The handover's sign: "&lt;name&gt;-up" (Handover).</summary>
    public static string UpName(string folder, string? testRoot) => Name(folder, testRoot) + "-up";
    public static string ShowName(string folder, string? testRoot) => Name(folder, testRoot) + "-show";

    readonly string folder;
    readonly string? testRoot;
    // (Windows: the thread that owns the mutex, and what it is asked to do)
    BlockingCollection<Action>? asks;
    Thread? owner;
    Mutex? held;
    EventWaitHandle? call;
    ManualResetEvent? quit;
    Thread? waiting;
    Action? come;
    // (Linux)
    FileStream? lockFile;
    Socket? listener;

    InstanceLock(string folder, string? testRoot) { this.folder = folder; this.testRoot = testRoot; }

    public bool Held => held != null || lockFile != null;

    /// <summary>The lock of this folder, or null when another launcher holds it. Where no lock can be made at all nobody is
    /// refused: the launcher must still start there (a lock that holds nothing is returned).</summary>
    public static InstanceLock? TryTake(string folder, string? testRoot)
    {
        var l = new InstanceLock(folder, testRoot);
        return l.Take() ? l : null;
    }

    bool Take()
    {
        if (Held) return true;
        if (!OperatingSystem.IsWindows()) return TakeFile();
        asks ??= StartOwner();
        var done = new TaskCompletionSource<bool>();
        asks.Add(() => done.SetResult(TakeMutex()));
        return done.Task.GetAwaiter().GetResult();
    }

    BlockingCollection<Action> StartOwner()
    {
        var q = new BlockingCollection<Action>();
        owner = new Thread(() => { foreach (var a in q.GetConsumingEnumerable()) a(); }) { IsBackground = true, Name = "Triforce instance lock" };
        owner.Start();
        return q;
    }

    bool TakeMutex()   // (on the owner thread)
    {
        var name = Name(folder, testRoot);
        Mutex m;
        try { m = new Mutex(false, name); }
        catch (UnauthorizedAccessException) { return false; }   // (it is there, and belongs to a launcher run as administrator)
        catch (Exception) { return true; }
        try
        {
            if (!m.WaitOne(0)) { m.Dispose(); return false; }
        }
        catch (AbandonedMutexException) { }   // (its holder ended without letting go: Windows hands it on, and it is ours)
        catch (Exception) { m.Dispose(); return true; }
        held = m;
        try
        {
            call = new EventWaitHandle(false, EventResetMode.AutoReset, name + "-show");
            call.Reset();   // (a call meant for the launcher before this one)
        }
        catch (Exception) { call = null; }
        if (come != null) Wait();
        return true;
    }

    /// <summary>The lock again after Release (a launcher that let go of it for its successor, which then did not start).</summary>
    public bool TakeAgain() => Take();

    /// <summary>What the holder does when a second launcher is started: called on a thread of its own, each time.</summary>
    public void Listen(Action onCall)
    {
        come = onCall;
        if (held != null || lockFile != null) Wait();
    }

    void Wait()
    {
        if (come == null || waiting != null) return;
        var act = come;
        if (OperatingSystem.IsWindows())
        {
            if (call == null) return;
            var (c, q) = (call, quit = new ManualResetEvent(false));
            waiting = new Thread(() =>
            {
                try { while (WaitHandle.WaitAny(new WaitHandle[] { q, c }) == 1) act(); }
                catch (Exception) { }
            }) { IsBackground = true, Name = "Triforce come forward" };
            waiting.Start();
            return;
        }
        var l = listener;
        if (l == null) return;
        waiting = new Thread(() =>
        {
            try
            {
                for (;;)
                {
                    using var s = l.Accept();
                    act();
                }
            }
            catch (Exception) { }
        }) { IsBackground = true, Name = "Triforce come forward" };
        waiting.Start();
    }

    /// <summary>Lets go of the folder: before the launcher that takes this one's place is started, and when the window has closed.</summary>
    public void Release()
    {
        try { quit?.Set(); waiting?.Join(2000); } catch (Exception) { }
        try { listener?.Dispose(); } catch (Exception) { }
        listener = null;
        waiting = null;
        quit = null;
        try { call?.Dispose(); } catch (Exception) { }
        call = null;
        if (lockFile != null)
        {
            try { lockFile.Dispose(); } catch (Exception) { }
            lockFile = null;
            try { File.Delete(SocketPath(folder, testRoot)); } catch (Exception) { }
        }
        if (held == null || asks == null) return;
        var done = new TaskCompletionSource();
        asks.Add(() =>
        {
            try { held?.ReleaseMutex(); } catch (Exception) { }
            held?.Dispose();
            held = null;
            done.SetResult();
        });
        done.Task.Wait(5000);
    }

    public void Dispose()
    {
        Release();
        asks?.CompleteAdding();
    }

    /// <summary>A second launcher asks the one that holds the folder to come to the front. False when there is nobody to ask (yet).</summary>
    public static bool CallForward(string folder, string? testRoot)
    {
        if (!OperatingSystem.IsWindows())
        {
            try
            {
                using var s = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
                s.Connect(new UnixDomainSocketEndPoint(SocketPath(folder, testRoot)));
                return true;
            }
            catch (Exception) { return false; }
        }
        try
        {
            if (!EventWaitHandle.TryOpenExisting(ShowName(folder, testRoot), out var c)) return false;
            using (c)
            {
                // (Windows lets only the program in front hand the front on: this one, just started by the player, may)
                try { AllowSetForegroundWindow(-1); } catch (Exception) { }
                return c.Set();
            }
        }
        catch (Exception) { return false; }
    }

    [DllImport("user32.dll")]
    static extern bool AllowSetForegroundWindow(int process);

    // ---- Linux ----

    static string RuntimeDir => Environment.GetEnvironmentVariable("XDG_RUNTIME_DIR") is { Length: > 0 } r && Directory.Exists(r) ? r : Path.GetTempPath();
    static string FileStem(string folder, string? testRoot) => "triforce-" + Fnv1a64(Same(folder) + (testRoot != null ? "|" + Same(testRoot) : "")).ToString("x16");
    static string LockPath(string folder, string? testRoot) => Path.Combine(RuntimeDir, FileStem(folder, testRoot) + ".lock");
    // (a unix socket's path is at most 108 bytes: the hash alone names it)
    static string SocketPath(string folder, string? testRoot) => Path.Combine(RuntimeDir, FileStem(folder, testRoot) + ".sock");

    bool TakeFile()
    {
        try { lockFile = new FileStream(LockPath(folder, testRoot), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); }
        catch (IOException) { return false; }   // (another launcher has it)
        catch (Exception) { return true; }        // (no lock can be made here: nobody is refused)
        try
        {
            var sock = SocketPath(folder, testRoot);
            try { File.Delete(sock); } catch (Exception) { }
            listener = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
            listener.Bind(new UnixDomainSocketEndPoint(sock));
            listener.Listen(4);
        }
        catch (Exception) { listener = null; }
        if (come != null) Wait();
        return true;
    }
}

/// <summary>The launcher replacing itself with a newer one, so that a new one that does not start is not what the player is left
/// with (old Guard.cs Handover). The new exe takes the running one's name, the running one keeps running under another (Windows
/// lets a running program be renamed, not written over), and the new one is started. The old one stays, out of sight, until the
/// new one gives a sign that its window is up (the named event "&lt;lock name&gt;-up"). A new launcher that cannot be started, or that
/// ends with an error's exit code before that sign, is taken back. One that gives no sign and goes on running is left alone.</summary>
public static class Handover
{
    /// <summary>Up: its window came up. Failed: it could not come up. Unclear: neither is known.</summary>
    public enum Outcome { Up, Failed, Unclear }

    /// <summary>The old launcher, before it starts the new one: the sign it will wait for. Null where there are no named events.</summary>
    public static EventWaitHandle? Expect(string folder, string? testRoot)
    {
        if (!OperatingSystem.IsWindows()) return null;
        return new EventWaitHandle(false, EventResetMode.ManualReset, InstanceLock.UpName(folder, testRoot));
    }

    /// <summary>The new launcher, once its window is up: tells the launcher that started it, when one waits to hear it. Without
    /// it an old Setup waits 30 s hidden and then quits (research/02 2.8 point 3).</summary>
    public static void Up(string folder, string? testRoot)
    {
        if (!OperatingSystem.IsWindows()) return;
        try
        {
            if (EventWaitHandle.TryOpenExisting(InstanceLock.UpName(folder, testRoot), out var up))
                using (up) up.Set();
        }
        catch (Exception) { }
    }

    /// <summary>What became of the launcher that was started, within the time given (polled every 100 ms).</summary>
    public static Outcome Watch(Process started, WaitHandle? up, TimeSpan limit)
    {
        try
        {
            for (var until = DateTime.UtcNow + limit; ;)
            {
                if (up != null) { if (up.WaitOne(100)) return Outcome.Up; }
                else started.WaitForExit(100);   // (no sign to wait for: only how it ends says anything)
                if (started.HasExited) return up != null && up.WaitOne(0) ? Outcome.Up : started.ExitCode != 0 ? Outcome.Failed : Outcome.Unclear;
                if (DateTime.UtcNow > until) return Outcome.Unclear;
            }
        }
        catch (Exception) { return Outcome.Unclear; }
    }

    /// <summary>The running launcher (self) steps aside as previous, and fresh takes its name. A second step that fails is undone:
    /// there is always a launcher under the name the shortcuts start.</summary>
    public static void Swap(string fresh, string self, string previous)
    {
        if (File.Exists(previous)) File.Delete(previous);
        File.Move(self, previous);
        try { File.Move(fresh, self); }
        catch (Exception)
        {
            try { File.Move(previous, self); } catch (Exception) { }
            throw;
        }
    }

    /// <summary>The launcher before (previous, which may be the program that calls this) back over the one that did not start, in
    /// one step. Tried for a few seconds: a virus scanner may still hold the new file. False when it could not be done.</summary>
    public static bool Restore(IPlatform platform, string previous, string self, int tries = 20)
    {
        for (var i = 0; i < tries; i++)
        {
            if (i > 0) Thread.Sleep(250);
            try
            {
                if (File.Exists(previous)) { platform.ReplaceFile(previous, self); return true; }
            }
            catch (Exception) { }
        }
        return false;
    }
}
