using System.Net;
using System.Net.Http.Headers;

namespace Triforce.Logic;

/// <summary>How far a download is: 0..1 (0 while the size is unknown), the bytes there, the whole size (0 unknown).</summary>
public readonly record struct DownloadProgress(double Fraction, long Bytes, long Total);

/// <summary>A file fetched over HTTP into place through &lt;file&gt;.part, and checked before it is used (old Guard.cs Download).
///
/// A download that was cut off (the connection, Cancel, the launcher closed) leaves its part file, and the next one goes on from
/// it with a Range request where the server answers those; a server that does not sends the whole file, and the part is written
/// over. A part is gone on from only as a part of the same file: &lt;file&gt;.part.of beside it names the address and the size it
/// was begun for and the server's mark for the file it sent (its ETag, else its date), which goes back with the request
/// (If-Range), so a file published again under the same address comes whole and is never joined to the beginning of the one
/// before. A part without that note, or from a server that marks its files in neither way, is fetched anew.
///
/// The file gets its name only once it has the size the release lists (else the one the server names), and a server that names
/// another size than the release's is not believed before its first byte.</summary>
public sealed class Downloader
{
    readonly TriforceContext ctx;
    public Downloader(TriforceContext ctx) { this.ctx = ctx; }

    /// <summary>How long the server may send nothing before the download is given up (its part stays to be gone on from). A read
    /// that waits on the network hears no cancellation: it is left waiting and its connection closed behind it, for Cancel too.</summary>
    public TimeSpan Stall { get; set; } = TimeSpan.FromSeconds(60);

    static void Remove(string file)
    {
        try { File.Delete(file); } catch (Exception) { }
    }

    static string Mb(long bytes) => (bytes >> 20).ToString();

    /// <summary>The server's mark for the file it sends, to hand back with If-Range: a strong ETag, else the date it was last changed.</summary>
    static string Mark(HttpResponseMessage response) =>
        response.Headers.ETag is { IsWeak: false } tag ? tag.ToString()
        : response.Content.Headers.LastModified is { } changed ? changed.UtcDateTime.ToString("r") : "";

    /// <summary>url to file, with progress. size is what the release lists for it, 0 when it lists none.</summary>
    public async Task ToFileAsync(HttpClient http, string url, string file, long size, Action<DownloadProgress>? progress, CancellationToken token)
    {
        var name = Path.GetFileName(file);
        var part = file + ".part";
        var note = part + ".of";
        long have = 0;
        RangeConditionHeaderValue? same = null;
        void Forget() { Remove(part); Remove(note); }
        void Say(double f, long b, long t) => progress?.Invoke(new DownloadProgress(f, b, t));
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(file))!);
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
            HttpResponseMessage response;
            try { response = await http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, either.Token).ConfigureAwait(false); }
            catch (OperationCanceledException) when (!token.IsCancellationRequested && stall.IsCancellationRequested)
            {
                throw new CutOffException("The server did not answer for " + (int)Stall.TotalSeconds + " seconds about " + name + ". Try again.");
            }
            var left = false;   // (the answer was left with a read still waiting on it)
            try
            {
                var range = response.StatusCode == HttpStatusCode.PartialContent ? response.Content.Headers.ContentRange : null;
                // (416: the part is no part of what the server has now. A range other than the one asked for: not to be joined on.
                // Either way the whole file is asked for, once)
                if (have > 0 && !again && (response.StatusCode == HttpStatusCode.RequestedRangeNotSatisfiable ||
                                           response.StatusCode == HttpStatusCode.PartialContent && !(range is { HasRange: true } && range.From == have)))
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
                if (total > 0 && ctx.SpaceProblem(name, total - have, file) is { } none) throw new NoRoomException(none);
                if (have == 0)
                {
                    Remove(part);
                    try { File.WriteAllLines(note, new[] { url, size.ToString(), Mark(response) }); } catch (Exception) { }
                }
                Say(total > 0 ? Math.Min(1.0, (double)have / total) : 0, have, total);

                var from = await response.Content.ReadAsStreamAsync(either.Token).ConfigureAwait(false);
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
                            await to.WriteAsync(buffer.AsMemory(0, n), CancellationToken.None).ConfigureAwait(false);
                            have += n;
                            // (a server that sends more than the file would fill the disk)
                            if (total > 0 && have > total) { tooMuch = true; break; }
                            if (DateTime.UtcNow - shown > TimeSpan.FromMilliseconds(150))
                            {
                                shown = DateTime.UtcNow;
                                Say(total > 0 ? Math.Min(1.0, (double)have / total) : 0, have, total);
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
                        if (ctx.Platform.IsDiskFull(e)) throw;
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
        Say(1, have, have);
    }

    /// <summary>The HttpClient every release request goes through: no overall timeout (a 300 MB download takes what it takes; the
    /// release check has its own 20 s), the launcher's User-Agent (old Shell.cs Http).</summary>
    public static HttpClient CreateHttp()
    {
        var http = new HttpClient { Timeout = Timeout.InfiniteTimeSpan };
        http.DefaultRequestHeaders.UserAgent.ParseAdd(BuildInfo.UserAgent);
        return http;
    }
}
