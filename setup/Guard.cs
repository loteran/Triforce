// What keeps a run of Setup from damaging an install. Nothing here needs the window or the PC's real install: each part is plain
// logic over what it is handed, and tests/test_setup_*.py compile this file alone and run it.
//   Instance   one Setup at a time for an install
//   Space      room on a disk, asked before it is needed
//   Download   a file fetched through a .part file that a cut download goes on from, checked before it is used
//   Partial    (Triforce) a copy written as <name>.partial and renamed into place once complete, never seen half done
//   SaveLock   (Triforce) the save's lock the game holds while it runs, held by Setup across a copy of a save
//   Report     what a player copies after a build that stopped, without their name in it
//   Handover   Setup replacing itself with a newer one, and taking its place back when that one does not start
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace TriforceSetup;

/// One Setup at a time for an install. Two of them on one folder each downloaded the tools into the same files, unpacked them over
/// each other and built into the same game folder (nx2/make_game.py stops a build it finds running on its work folder, so the
/// first Setup's build ended without a reason). The first Setup holds a named mutex for as long as it runs; Windows lets go of it
/// when the process ends, however it ends. A second start for the same folder asks the first to come to the front (a named event
/// the first one waits on) and ends. Both names are made from the folder Setup works in and, in a test run (TRIFORCE_TEST_ROOT),
/// from the test folder too: a test run never shares a lock with the PC's real install.
static class Instance
{
    static Mutex? held;
    static EventWaitHandle? call;     // (set by a second Setup: "come to the front")
    static ManualResetEvent? quit;    // (ends the thread that waits for that)
    static Thread? waiting;
    static Action? come;
    static (string Folder, string? TestRoot)? where;

    [DllImport("user32.dll")]
    static extern bool AllowSetForegroundWindow(int process);

    /// One spelling for a folder, however it was typed
    static string Same(string path)
    {
        try { path = Path.GetFullPath(path); } catch (Exception) { }
        return path.TrimEnd('\\', '/').ToUpperInvariant();
    }

    /// The lock's name for a folder: a number made from the whole path (and the test folder's), then the end of the path for
    /// whoever looks at it. (Local\: one user's session. An install is one user's, HKCU and %LOCALAPPDATA%)
    public static string Name(string folder, string? testRoot)
    {
        var text = Same(folder) + (testRoot != null ? "|" + Same(testRoot) : "");
        var h = 14695981039346656037UL;   // (FNV-1a, 64 bits: a name, not a secret)
        foreach (var c in text) { h ^= c; h *= 1099511628211UL; }
        var tail = new string(Same(folder).Select(c => char.IsLetterOrDigit(c) ? c : '-').ToArray());
        return @"Local\TriforceSetup-" + h.ToString("x16") + "-" + (tail.Length > 60 ? tail.Substring(tail.Length - 60) : tail);
    }

    /// True when this process is now the one Setup of that folder (or was already), false when another one is. Where no lock
    /// can be made at all nobody is refused: Setup must still start there. Taken and let go on the same thread (a mutex is a
    /// thread's).
    public static bool Take(string folder, string? testRoot)
    {
        if (held != null) return true;
        where = (folder, testRoot);
        var name = Name(folder, testRoot);
        Mutex m;
        try { m = new Mutex(false, name); }
        catch (UnauthorizedAccessException) { return false; }   // (it is there, and belongs to a Setup run as administrator)
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
            call.Reset();   // (a call meant for the Setup before this one)
        }
        catch (Exception) { call = null; }
        if (come != null) Wait();
        return true;
    }

    /// The lock again after Release (a Setup that let go of it for its successor, which then did not start)
    public static bool TakeAgain() => where is not { } w || Take(w.Folder, w.TestRoot);

    /// What the one Setup does when a second one is started: called on a thread of its own, each time
    public static void Listen(Action onCall)
    {
        come = onCall;
        if (held != null) Wait();
    }

    static void Wait()
    {
        if (call == null || come == null || waiting != null) return;
        var (c, q, act) = (call, quit = new ManualResetEvent(false), come);
        waiting = new Thread(() =>
        {
            try { while (WaitHandle.WaitAny(new WaitHandle[] { q, c }) == 1) act(); }
            catch (Exception) { }
        }) { IsBackground = true };
        waiting.Start();
    }

    /// Lets go of the folder: before the Setup that takes this one's place is started, and when the window has closed
    public static void Release()
    {
        try { quit?.Set(); waiting?.Join(2000); } catch (Exception) { }
        waiting = null;
        quit = null;
        try { call?.Dispose(); } catch (Exception) { }
        call = null;
        if (held == null) return;
        // (on another thread than the one that took it Windows refuses: then closing it is all there is, and the end of the
        // process lets go)
        try { held.ReleaseMutex(); } catch (Exception) { }
        held.Dispose();
        held = null;
    }

    /// A second Setup asks the one that holds the folder to come to the front. False when there is nobody to ask (yet)
    public static bool CallForward(string folder, string? testRoot)
    {
        try
        {
            if (!EventWaitHandle.TryOpenExisting(Name(folder, testRoot) + "-show", out var c)) return false;
            using (c)
            {
                // (Windows lets only the program in front hand the front on: this one, just started by the player, may)
                try { AllowSetForegroundWindow(-1); } catch (Exception) { }
                return c.Set();
            }
        }
        catch (Exception) { return false; }
    }
}

/// Room on a disk, asked before it is needed. A download or an unpacking that ran out of room stopped somewhere in the middle
/// with Windows' own words for it and left half of itself behind; asked first, the player is told how much it takes and how
/// much there is, before anything is written.
static class Space
{
    /// What a disk keeps free beyond what is about to be written to it
    public const long Margin = 32L << 20;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool GetDiskFreeSpaceExW(string folder, out ulong forCaller, out ulong total, out ulong free);

    /// The free bytes of the disk that holds a folder, asked of the nearest folder above it that is there (the folder itself may
    /// still be to make); null when that cannot be asked. (A test puts its own answer here.)
    public static Func<string, long?> Free = Ask;

    static long? Ask(string folder)
    {
        try
        {
            for (var d = Path.GetFullPath(folder); !string.IsNullOrEmpty(d); d = Path.GetDirectoryName(d))
                if (Directory.Exists(d))
                    return GetDiskFreeSpaceExW(d, out var mine, out _, out _) ? (long)Math.Min(mine, (ulong)long.MaxValue) : null;
        }
        catch (Exception) { }
        return null;
    }

    /// The disk of a folder as the player knows it: C:\
    public static string Disk(string folder)
    {
        try { return Path.GetPathRoot(Path.GetFullPath(folder)) is { Length: > 0 } root ? root : folder; }
        catch (Exception) { return folder; }
    }

    /// Null when the free bytes are room for need more of them and the margin, or are not known; else the sentence that says
    /// there is no room for what, how much it takes and how much is free (what it takes rounded up and what is free rounded
    /// down, so the two numbers never read the same)
    public static string? Problem(string what, long need, long? free, string disk)
    {
        need = Math.Max(0, need) + Margin;
        if (free is not { } f || f >= need) return null;
        return "There is no room for " + what + " on " + disk + ": it takes " + ((need + (1 << 20) - 1) >> 20) + " MB there, and " + (Math.Max(0, f) >> 20) +
               " MB are free. Make room and try again.";
    }

    /// Is this the error of a disk that filled while it was written to?
    public static bool Full(Exception? e)
    {
        for (; e != null; e = e.InnerException)
            if (e is IOException && (uint)e.HResult >> 16 == 0x8007 && ((e.HResult & 0xFFFF) == 112 || (e.HResult & 0xFFFF) == 39))
                return true;   // (ERROR_DISK_FULL, ERROR_HANDLE_DISK_FULL)
        return false;
    }
}

/// There is no room for what is about to be written (Space.Problem's sentence), said before any of it is
sealed class NoRoomException : IOException
{
    public NoRoomException(string message) : base(message) { }
}

/// A download stopped before its end (the connection was cut, went silent or closed early): its part is kept, and the next try
/// goes on from it
sealed class CutOffException : IOException
{
    public CutOffException(string message, Exception? inner = null) : base(message, inner) { }
}

/// A file fetched over HTTP into place through <file>.part, and checked before it is used.
///
/// A download that was cut off (the connection, Cancel, Setup closed) leaves its part file, and the next one goes on from it with
/// a Range request where the server answers those; a server that does not sends the whole file, and the part is written over.
/// A part is gone on from only as a part of the same file: <file>.part.of beside it names the address and the size it was begun
/// for and the server's mark for the file it sent (its ETag, else its date), which goes back with the request (If-Range), so a
/// file published again under the same address comes whole and is never joined to the beginning of the one before. A part
/// without that note, or from a server that marks its files in neither way, is fetched anew.
///
/// The file gets its name only once it has the size the release lists (else the one the server names), and a server that names
/// another size than the release's is not believed before its first byte.
static class Download
{
    /// How long the server may send nothing before the download is given up (its part stays to be gone on from). A read that
    /// waits on the network hears no cancellation: it is left waiting and its connection closed behind it, for Cancel too.
    public static TimeSpan Stall = TimeSpan.FromSeconds(60);

    static void Remove(string file)
    {
        try { File.Delete(file); } catch (Exception) { }
    }

    static string Mb(long bytes) => (bytes >> 20).ToString();

    /// The server's mark for the file it sends, to hand back with If-Range: a strong ETag, else the date it was last changed
    static string Mark(HttpResponseMessage response) =>
        response.Headers.ETag is { IsWeak: false } tag ? tag.ToString()
        : response.Content.Headers.LastModified is { } changed ? changed.UtcDateTime.ToString("r") : "";

    /// url to file, with progress (0..1, and the bytes that are there). size is what the release lists for it, 0 when it lists none
    public static async Task ToFile(HttpClient http, string url, string file, long size, Action<double, long> progress, CancellationToken token)
    {
        var name = Path.GetFileName(file);
        var part = file + ".part";
        var note = part + ".of";
        long have = 0;
        RangeConditionHeaderValue? same = null;
        void Forget() { Remove(part); Remove(note); }
        if (File.Exists(part))
        {
            // (what the part is a part of: only the same address, size and mark are gone on from, and never a part as long as the file)
            string[] of = { };
            try { of = File.ReadAllLines(note); } catch (Exception) { }
            var length = new FileInfo(part).Length;
            if (of.Length >= 3 && of[0] == url && of[1] == size.ToString() && RangeConditionHeaderValue.TryParse(of[2], out same) && length > 0 && (size <= 0 || length < size))
                have = length;
            else Forget();
        }

        using var stall = new CancellationTokenSource();
        using var either = CancellationTokenSource.CreateLinkedTokenSource(token, stall.Token);
        for (var again = false; ; again = true)
        {
            using var request = new HttpRequestMessage(HttpMethod.Get, url);
            if (have > 0)
            {
                request.Headers.Range = new RangeHeaderValue(have, null);
                request.Headers.IfRange = same;
            }
            stall.CancelAfter(Stall);
            var response = await http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, either.Token).ConfigureAwait(false);
            var left = false;   // (the answer was left with a read still waiting on it)
            try
            {
                var range = response.StatusCode == HttpStatusCode.PartialContent ? response.Content.Headers.ContentRange : null;
                // (416: the part is no part of what the server has now. A range other than the one asked for: not to be joined on.
                // Either way the whole file is asked for, once)
                if (have > 0 && !again && (response.StatusCode == HttpStatusCode.RequestedRangeNotSatisfiable || response.StatusCode == HttpStatusCode.PartialContent && !(range is { HasRange: true } && range.From == have)))
                {
                    Forget();
                    have = 0;
                    continue;
                }
                response.EnsureSuccessStatusCode();
                if (response.StatusCode == HttpStatusCode.PartialContent && (have == 0 || !(range is { HasRange: true } && range.From == have)))
                    throw new IOException("The server sent a piece of " + name + " that was not asked for.");
                // (200 to a request for the rest: the server knows no ranges, or the file is another one now. The whole file comes)
                if (response.StatusCode != HttpStatusCode.PartialContent) have = 0;

                // the whole file's size: the release's, which the server must not contradict; else the server's
                var told = have > 0 ? range!.Length ?? have + response.Content.Headers.ContentLength : response.Content.Headers.ContentLength;
                if (size > 0 && told is { } other && other != size)
                {
                    Forget();
                    throw new IOException("The server offers " + other + " bytes as " + name + ", and the release lists " + size + ". Nothing was downloaded.");
                }
                var total = size > 0 ? size : told ?? 0;
                if (total > 0 && Space.Problem(name, total - have, Space.Free(file), Space.Disk(file)) is { } none) throw new NoRoomException(none);
                if (have == 0)
                {
                    Remove(part);
                    try { File.WriteAllLines(note, new[] { url, size.ToString(), Mark(response) }); } catch (Exception) { }
                }
                progress(total > 0 ? Math.Min(1.0, (double)have / total) : 0, have);

                var from = await response.Content.ReadAsStreamAsync().ConfigureAwait(false);
                var gone = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
                var tooMuch = false;
                using (var to = new FileStream(part, have > 0 ? FileMode.Append : FileMode.Create, FileAccess.Write, FileShare.None))
                using (either.Token.Register(() => gone.TrySetResult(true)))
                {
                    var buffer = new byte[1 << 20];
                    var shown = DateTime.MinValue;
                    try
                    {
                        for (;;)
                        {
                            stall.CancelAfter(Stall);
                            var read = from.ReadAsync(buffer, 0, buffer.Length, either.Token);
                            if (await Task.WhenAny(read, gone.Task).ConfigureAwait(false) != read)
                            {
                                // (Cancel, or nothing came: the read is left where it waits, whatever becomes of it)
                                left = true;
                                _ = read.ContinueWith(t => t.Exception, TaskContinuationOptions.OnlyOnFaulted);
                                throw new OperationCanceledException();
                            }
                            var n = await read.ConfigureAwait(false);
                            stall.CancelAfter(Timeout.InfiniteTimeSpan);
                            if (n <= 0) break;
                            await to.WriteAsync(buffer, 0, n).ConfigureAwait(false);
                            have += n;
                            // (a server that sends more than the file would fill the disk)
                            if (total > 0 && have > total) { tooMuch = true; break; }
                            if (DateTime.UtcNow - shown > TimeSpan.FromMilliseconds(150))
                            {
                                shown = DateTime.UtcNow;
                                progress(total > 0 ? Math.Min(1.0, (double)have / total) : 0, have);
                            }
                        }
                    }
                    catch (Exception e)
                    {
                        if (token.IsCancellationRequested) throw new OperationCanceledException(token);
                        var of = total > 0 ? " of " + Mb(total) : "";
                        if (stall.IsCancellationRequested)
                            throw new CutOffException("The download of " + name + " stopped after " + Mb(have) + of + " MB: nothing more came for " + (int)Stall.TotalSeconds +
                                                      " seconds. Try again: it goes on from there.");
                        if (Space.Full(e)) throw;
                        throw new CutOffException("The download of " + name + " was cut off after " + Mb(have) + of + " MB. Try again: it goes on from there.", e);
                    }
                }
                if (tooMuch)
                {
                    Forget();
                    throw new IOException("The server sent more than the " + total + " bytes of " + name + ". Nothing of it was kept.");
                }
                if (total > 0 && have < total)
                    throw new CutOffException("The download of " + name + " ended early, after " + Mb(have) + " of " + Mb(total) + " MB. Try again: it goes on from there.");
                break;
            }
            finally
            {
                // (closing an answer can wait for the network as its read did: one that was left is closed where nobody waits for that)
                if (left) _ = Task.Run(() => { try { response.Dispose(); } catch (Exception) { } });
                else response.Dispose();
            }
        }

        // (before it is used: what is on the disk is what was counted)
        var there = new FileInfo(part).Length;
        if (there != have)
        {
            Forget();
            throw new IOException(name + " is " + there + " bytes on the disk where " + have + " were downloaded. Nothing of it was kept.");
        }
        if (File.Exists(file)) File.Delete(file);
        File.Move(part, file);
        Remove(note);
        progress(1, have);
    }
}

/// (Triforce) A copy that is never seen half done. Setup copied itself into the install folder and a save over the one there
/// file by file, in place: a full disk, a file held open or Setup closed in the middle left a Setup that does not start under
/// the name every shortcut starts, or half a save. Now the copy is written as <name>.partial beside its place and renamed into
/// it once complete; what was there is replaced in one step (a file) or moved aside as <name>.old and removed after (a
/// folder), and moved back when the new one cannot go in. A failure removes the partial and leaves what was there as it was.
static class Partial
{
    public const string Suffix = ".partial", Aside = ".old";

    /// (testing) called with the partial once it is complete, before it goes into its place: a test checks what holds then, or
    /// throws to stand for a failure there
    public static Action<string>? Copied { get; set; }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool MoveFileExW(string from, string to, int flags);

    /// A file or a folder and what is in it, gone; quietly when it cannot be
    public static void Remove(string path)
    {
        try
        {
            if (File.Exists(path)) { File.SetAttributes(path, FileAttributes.Normal); File.Delete(path); }
            else if (Directory.Exists(path)) Directory.Delete(path, true);
        }
        catch (Exception) { }
    }

    /// from copied to to, through to.partial: an older to is replaced in one step only once the copy is complete
    public static void CopyFile(string from, string to)
    {
        var partial = to + Suffix;
        try
        {
            Remove(partial);
            File.Copy(from, partial, false);
            Copied?.Invoke(partial);
            if (!MoveFileExW(partial, to, 1 | 8))   // (MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                throw new IOException("Could not put " + to + " in place (Windows error " + Marshal.GetLastWin32Error() + ").");
        }
        catch (Exception)
        {
            Remove(partial);
            throw;
        }
    }

    /// The folder from copied as to.partial (one there from a copy that was cut off is removed first); its path. A failure
    /// removes what it wrote
    public static string Stage(string from, string to)
    {
        var partial = to + Suffix;
        try
        {
            Remove(partial);
            Tree(from, partial);
            return partial;
        }
        catch (Exception)
        {
            Remove(partial);
            throw;
        }
    }

    static void Tree(string from, string to)
    {
        Directory.CreateDirectory(to);
        foreach (var f in Directory.GetFiles(from)) File.Copy(f, Path.Combine(to, Path.GetFileName(f)), false);
        foreach (var d in Directory.GetDirectories(from)) Tree(d, Path.Combine(to, Path.GetFileName(d)));
    }

    /// A complete partial folder put in to's place: the folder there moved aside as to.old first and removed after, or moved
    /// back when the new one cannot go in (the partial is then removed)
    public static void SwapIn(string partial, string to)
    {
        var old = to + Aside;
        try
        {
            Copied?.Invoke(partial);
            Remove(old);
            if (Directory.Exists(to)) Directory.Move(to, old);
            try { Directory.Move(partial, to); }
            catch (Exception)
            {
                try { if (!Directory.Exists(to) && Directory.Exists(old)) Directory.Move(old, to); } catch (Exception) { }
                throw;
            }
        }
        catch (Exception)
        {
            Remove(partial);
            throw;
        }
        Remove(old);
    }

    /// What a copy that was cut off (Setup closed in the middle of it) left beside to: the folder moved aside goes back when to
    /// is gone, and a partial goes
    public static void Recover(string to)
    {
        try { if (!Directory.Exists(to) && Directory.Exists(to + Aside)) Directory.Move(to + Aside, to); } catch (Exception) { }
        if (Directory.Exists(to)) Remove(to + Aside);
        Remove(to + Suffix);
    }
}

/// (Triforce) The save's lock between the game and Setup. The game holds <user>\save.lock open without sharing from its first
/// save mount to its end (host/save_tx.h save_tx_lock); NX2navi holds <profile>\instance.lock the same way. Setup took a
/// look at the running programs and then copied: a game started in between (the copy, or the player's choice before it)
/// mounted a save half copied, and wrote it over the copy when it closed. Taken by Setup across the whole copy, the lock says
/// for certain whether the game runs, and a game started meanwhile waits for it before it mounts its save.
static class SaveLock
{
    public const string Running = "The game is running: quit it first. It keeps its save open while it runs, and would write over the copy.";

    /// The lock of a save folder <user>\save\<uid> (or a commit waiting in <user>\save-tx\<uid>\ready-<n>): <user>\save.lock, or
    /// instance.lock in NX2navi's profile folder; null for another kind of save folder (an emulator's), which has none
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

    /// The lock of a save folder, held until it is disposed (an open handle that shares nothing). Null when the folder has none
    /// or it cannot be made there (nothing is locked: the copy goes on as before). InvalidOperationException (Running) when a
    /// game holds it
    public static FileStream? Take(string save)
    {
        if (PathOf(save) is not { } path) return null;
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            return new FileStream(path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        }
        catch (IOException e) when ((e.HResult & 0xFFFF) == 32 || (e.HResult & 0xFFFF) == 33)   // (ERROR_SHARING_VIOLATION, ERROR_LOCK_VIOLATION)
        {
            throw new InvalidOperationException(Running);
        }
        catch (Exception) { return null; }
    }
}

/// What a player copies after a build that stopped, to paste where they ask for help: what Setup knows of the build, the build's
/// last lines and setup.log's. The folder of the player's Windows account, which carries their name and is in most paths of a
/// log, reads %USERPROFILE% in it.
static class Report
{
    /// How many of its last lines each log gives
    public const int Lines = 200;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern uint GetShortPathNameW(string path, StringBuilder into, uint size);

    /// A folder's short name (C:\Users\LONGNA~1), which programs print for temporary files, when it has another one than its own
    public static string? Short(string folder)
    {
        try
        {
            var into = new StringBuilder(1024);
            var n = GetShortPathNameW(folder, into, (uint)into.Capacity);
            return n > 0 && n < into.Capacity && !string.Equals(into.ToString(), folder, StringComparison.OrdinalIgnoreCase) ? into.ToString() : null;
        }
        catch (Exception) { return null; }
    }

    /// The last lines of a text, at most so many, without the empty ones at its end
    public static string Tail(string text, int lines)
    {
        var all = text.Replace("\r", "").TrimEnd('\n', ' ').Split('\n');
        return string.Join(Environment.NewLine, all.Skip(Math.Max(0, all.Length - lines)));
    }

    /// The text with each of the folders (the account's own, and its short name) read as %USERPROFILE%: in any case of its
    /// letters, with either slash or doubled ones between its parts, as a build's tools print paths. A folder that only begins
    /// with the same letters (C:\Users\Ann and C:\Users\Anna) is another one and stays.
    public static string NoName(string text, params string?[] folders)
    {
        foreach (var folder in folders.Where(f => !string.IsNullOrWhiteSpace(f)).OrderByDescending(f => f!.Length))
        {
            var parts = folder!.Split(new[] { '\\', '/' }, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length < 2) continue;   // (a whole drive is nobody's name)
            var pattern = (folder.StartsWith("\\") || folder.StartsWith("/") ? @"[\\/]+" : "") + string.Join(@"[\\/]+", parts.Select(Regex.Escape)) + @"(?![\p{L}\p{N}])";
            text = Regex.Replace(text, pattern, "%USERPROFILE%", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
        }
        return text;
    }

    /// The report: what Setup knows, then the last lines of what the build said and of setup.log
    public static string Compose(IEnumerable<string> known, string build, string log, params string?[] folders)
    {
        var s = new StringBuilder();
        foreach (var line in known) s.AppendLine(line);
        s.AppendLine().AppendLine("---- the build's last lines ----").AppendLine(Tail(build, Lines));
        s.AppendLine().AppendLine("---- setup.log's last lines ----").AppendLine(Tail(log, Lines));
        return NoName(s.ToString(), folders);
    }
}

/// The Setup an update brought could not start, and what was done about it
sealed class NotStartedException : Exception
{
    public NotStartedException(string message) : base(message) { }
}

/// Setup replacing itself with a newer one, so that a new one that does not start is not what the player is left with.
///
/// The new exe takes the running one's name, the running one keeps running under another (Windows lets a running program be
/// renamed, not written over), and the new one is started. No second program watches over this: the old Setup itself stays,
/// out of sight, until the new one gives a sign that its window is up (a named event, named as Instance's lock is). A new
/// Setup that cannot be started, or that ends with an error's exit code before that sign, is taken back: the old exe goes
/// over it under the old name in one step, and the old Setup shows itself again. One that gives no sign and goes on running
/// is left alone (slow to start, or a release that gives none): only what is known to have failed is undone.
static class Handover
{
    /// Up: its window came up. Failed: it could not come up. Unclear: neither is known (it runs on without the sign, or ended
    /// without an error, as a second Setup does that found another one open)
    public enum Outcome { Up, Failed, Unclear }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool MoveFileExW(string from, string to, int flags);

    static string Sign(string folder, string? testRoot) => Instance.Name(folder, testRoot) + "-up";

    /// The old Setup, before it starts the new one: the sign it will wait for
    public static EventWaitHandle Expect(string folder, string? testRoot) => new(false, EventResetMode.ManualReset, Sign(folder, testRoot));

    /// The new Setup, once its window is up: tells the Setup that started it, when one waits to hear it
    public static void Up(string folder, string? testRoot)
    {
        try
        {
            if (EventWaitHandle.TryOpenExisting(Sign(folder, testRoot), out var up))
                using (up) up.Set();
        }
        catch (Exception) { }
    }

    /// What became of the Setup that was started, within the time given
    public static Outcome Watch(Process started, WaitHandle up, TimeSpan limit)
    {
        try
        {
            for (var until = DateTime.UtcNow + limit; ;)
            {
                if (up.WaitOne(100)) return Outcome.Up;
                if (started.HasExited) return up.WaitOne(0) ? Outcome.Up : started.ExitCode != 0 ? Outcome.Failed : Outcome.Unclear;
                if (DateTime.UtcNow > until) return Outcome.Unclear;
            }
        }
        catch (Exception) { return Outcome.Unclear; }
    }

    /// The running Setup (self) steps aside as previous, and fresh takes its name. A second step that fails is undone: there is
    /// always a Setup under the name the shortcuts start
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

    /// The Setup before (previous, which may be the program that calls this) back over the one that did not start, in one step,
    /// so that no moment is without a Setup under that name. Tried for a few seconds: a virus scanner may still hold the new
    /// file. False when it could not be done, and then nothing was changed
    public static bool Restore(string previous, string self, int tries = 20)
    {
        for (var i = 0; i < tries; i++)
        {
            if (i > 0) Thread.Sleep(250);
            try
            {
                if (File.Exists(previous) && MoveFileExW(previous, self, 1 | 8)) return true;   // (MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
            }
            catch (Exception) { }
        }
        return false;
    }
}
