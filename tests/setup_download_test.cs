// tests/test_setup_download.py: setup/Guard.cs's Download against a server that is an object in this program (no network, no
// socket): it answers Range and If-Range as a file server does, and can cut the connection, go silent, end early, send too much
// or name another size. Files go to a temporary folder. No Setup runs and no install is touched.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Threading;
using System.Threading.Tasks;

namespace TriforceSetup;

/// The body of an answer, handed out in small reads
sealed class Feed : Stream
{
    readonly byte[] bytes;
    readonly int cut, silent;
    readonly bool stubborn;
    readonly TaskCompletionSource<bool> gone = new();
    int at;

    /// cut: the connection breaks once this many bytes went out. silent: from there on nothing comes, until the connection is
    /// closed; stubborn: not even then, and closing it takes six seconds (a connection that waits for the network to close)
    public Feed(byte[] bytes, int cut, int silent, bool stubborn) { this.bytes = bytes; this.cut = cut; this.silent = silent; this.stubborn = stubborn; }

    public override async Task<int> ReadAsync(byte[] buffer, int offset, int count, CancellationToken token)
    {
        if (gone.Task.IsCompleted) throw new ObjectDisposedException("the connection");
        if (cut >= 0 && at >= cut) throw new IOException("An existing connection was forcibly closed by the remote host.");
        if (silent >= 0 && at >= silent)
        {
            await (stubborn ? new TaskCompletionSource<bool>().Task : gone.Task).ConfigureAwait(false);
            throw new IOException("The request was aborted.");
        }
        await Task.Yield();
        var n = Math.Min(Math.Min(count, 7919), bytes.Length - at);
        if (cut >= 0) n = Math.Min(n, cut - at);
        if (silent >= 0) n = Math.Min(n, silent - at);
        Array.Copy(bytes, at, buffer, offset, n);
        at += n;
        return n;
    }

    public override int Read(byte[] buffer, int offset, int count) => ReadAsync(buffer, offset, count, CancellationToken.None).GetAwaiter().GetResult();
    protected override void Dispose(bool disposing)
    {
        if (stubborn && !gone.Task.IsCompleted) Thread.Sleep(6000);
        gone.TrySetResult(true);
        base.Dispose(disposing);
    }
    public override bool CanRead => true;
    public override bool CanSeek => false;
    public override bool CanWrite => false;
    public override long Length => throw new NotSupportedException();
    public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }
    public override void Flush() { }
    public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
    public override void SetLength(long value) => throw new NotSupportedException();
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
}

/// A file server, as far as Download can tell
sealed class Server : HttpMessageHandler
{
    public byte[] Body = { };
    public string? Tag = "\"one\"";          // (its ETag for Body; W/"..." for a weak one)
    public DateTimeOffset? Changed;          // (its Last-Modified)
    public bool Ranges = true;               // (answers a request for the rest of the file)
    public bool WrongRange;                  // (answers it once with another piece than was asked for)
    public int Cut = -1, Silent = -1;        // (see Feed: counted in the bytes of one answer)
    public bool Stubborn;                    // (see Feed)
    public int Short;                        // (ends the answer this many bytes early, without an error)
    public int Extra;                        // (sends this many bytes more than the file)
    public bool NoLength;                    // (names no Content-Length)
    public bool NeverAnswers;                // (takes the request and says nothing)
    public HttpStatusCode? Status;           // (answers everything with this)
    public readonly List<(long? From, string? IfRange)> Asked = new();

    string? Mark => Tag != null && !Tag.StartsWith("W/") ? Tag : Changed?.UtcDateTime.ToString("r");

    protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken token)
    {
        var from = request.Headers.Range?.Ranges.First().From;
        var ifRange = request.Headers.IfRange?.ToString();
        Asked.Add((from, ifRange));
        if (NeverAnswers)
        {
            var never = new TaskCompletionSource<HttpResponseMessage>();
            token.Register(() => never.TrySetCanceled());
            return never.Task;
        }
        if (Status is { } status) return Task.FromResult(new HttpResponseMessage(status) { Content = new ByteArrayContent(new byte[0]) });
        var response = new HttpResponseMessage(HttpStatusCode.OK);
        var send = Body;
        if (from is { } start && Ranges && (ifRange == null || ifRange == Mark))
        {
            if (start >= Body.Length)
                return Task.FromResult(new HttpResponseMessage(HttpStatusCode.RequestedRangeNotSatisfiable) { Content = new ByteArrayContent(new byte[0]) });
            if (WrongRange) { WrongRange = false; start++; }
            response.StatusCode = HttpStatusCode.PartialContent;
            send = Body.Skip((int)start).ToArray();
            response.Content = new StreamContent(new Feed(send, Cut, Silent, Stubborn));
            response.Content.Headers.ContentRange = new ContentRangeHeaderValue(start, Body.Length - 1, Body.Length);
        }
        else
        {
            if (Extra > 0) send = Body.Concat(new byte[Extra]).ToArray();
            response.Content = new StreamContent(new Feed(Short > 0 ? send.Take(send.Length - Short).ToArray() : send, Cut, Silent, Stubborn));
        }
        if (!NoLength) response.Content.Headers.ContentLength = Extra > 0 ? Body.Length : send.Length;
        if (Tag != null) response.Headers.TryAddWithoutValidation("ETag", Tag);
        if (Changed != null) response.Content.Headers.LastModified = Changed;
        return Task.FromResult(response);
    }
}

static class DownloadTest
{
    static int checks;
    static void Check(bool ok, string what)
    {
        if (!ok) throw new Exception(what);
        checks++;
    }

    static byte[] Bytes(int n, int seed)
    {
        var b = new byte[n];
        new Random(seed).NextBytes(b);
        return b;
    }

    static string dir = "";
    static int serial;
    const string Url = "https://releases.test/triforce/downloads/releases/download/v9.9.9/Triforce-Tools.zip";

    /// One download in a folder of its own: the server, the file's place, and what was shown of its progress
    sealed class Case
    {
        public readonly Server Server = new() { Body = Bytes(300000, 1) };
        public readonly string File = Path.Combine(Directory.CreateDirectory(Path.Combine(dir, "case-" + ++serial)).FullName, "Triforce-Tools-v9.9.9.zip");
        public readonly List<(double F, long Have)> Shown = new();
        public string Part => File + ".part";
        public string Note => File + ".part.of";
        public long PartLength => System.IO.File.Exists(Part) ? new FileInfo(Part).Length : -1;

        public Exception? Get(long? size = null, CancellationToken token = default, string url = Url)
        {
            Shown.Clear();
            using var http = new HttpClient(Server, false) { Timeout = Timeout.InfiniteTimeSpan };
            try { Download.ToFile(http, url, File, size ?? Server.Body.Length, (f, have) => { lock (Shown) Shown.Add((f, have)); }, token).GetAwaiter().GetResult(); }
            catch (Exception e) { return e; }
            return null;
        }

        /// The file is there, whole and the server's, with nothing left beside it
        public bool Whole => System.IO.File.Exists(File) && System.IO.File.ReadAllBytes(File).SequenceEqual(Server.Body) && !System.IO.File.Exists(Part) && !System.IO.File.Exists(Note);
        /// Nothing usable is there under the file's name
        public bool NoFile => !System.IO.File.Exists(File);

        /// A download cut off after so many bytes: the part and its note stay
        public Case CutAt(int bytes)
        {
            Server.Cut = bytes;
            var e = Get();
            Check(e is CutOffException && e.Message.Contains("cut off") && e.Message.Contains("goes on from there"), "a cut connection is not told as that: " + e);
            Check(NoFile && PartLength == bytes && System.IO.File.Exists(Note), "a cut download left a file under its final name, or not its part (" + PartLength + " bytes) and its note");
            Server.Cut = -1;
            return this;
        }
    }

    static int Main()
    {
        dir = Path.Combine(Path.GetTempPath(), "triforce-download-test-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        var realFree = Space.Free;
        var realStall = Download.Stall;
        try
        {
            // a whole download
            var c = new Case();
            Check(c.Get() == null && c.Whole, "a plain download is not the server's file, or left something beside it");
            Check(c.Server.Asked.Count == 1 && c.Server.Asked[0].From == null, "a first download asked for a piece");
            Check(c.Shown.Count >= 2 && c.Shown.Last() == (1.0, 300000) && c.Shown.All(s => s.F >= 0 && s.F <= 1), "the progress does not end at the whole file");
            System.IO.File.WriteAllText(c.File, "an older file of that name");
            Check(c.Get() == null && c.Whole, "a file that was there under the name was not replaced");

            // cut off, and gone on from where it stopped
            c = new Case().CutAt(100000);
            Check(c.Get() == null && c.Whole, "a download that went on from its part is not the server's file");
            Check(c.Server.Asked.Last() == (100000, "\"one\""), "the second try did not ask for the rest, with the server's mark for the file: " + c.Server.Asked.Last());
            Check(c.Shown.First().Have == 100000 && Math.Abs(c.Shown.First().F - 1 / 3.0) < 0.01, "the progress of a download that goes on does not start where it stopped");
            // (twice)
            c = new Case().CutAt(100000);
            c.Server.Cut = 50000;
            Check(c.Get() is IOException && c.PartLength == 150000 && c.NoFile, "a second cut lost what was there");
            c.Server.Cut = -1;
            Check(c.Get() == null && c.Whole && c.Server.Asked.Last().From == 150000, "a download cut twice is not whole");
            // (a server that marks its files by their date alone)
            c = new Case();
            c.Server.Tag = null;
            c.Server.Changed = new DateTimeOffset(2026, 11, 14, 9, 30, 0, TimeSpan.Zero);
            c.CutAt(120000);
            Check(c.Get() == null && c.Whole && c.Server.Asked.Last() == (120000, "Sat, 14 Nov 2026 09:30:00 GMT"), "a part was not gone on from by the file's date: " + c.Server.Asked.Last());

            // never joined to another file's beginning
            c = new Case().CutAt(100000);
            c.Server.Body = Bytes(300000, 2);   // (published again under the same address, the same size)
            c.Server.Tag = "\"two\"";
            Check(c.Get() == null && c.Whole, "a file published again was joined to the beginning of the one before");
            c = new Case().CutAt(100000);
            c.Server.Ranges = false;   // (a server that sends the whole file whatever is asked)
            Check(c.Get() == null && c.Whole && c.Server.Asked.Last().From == 100000, "the whole file of a server without ranges was added to the part");
            c = new Case();
            c.Server.Tag = "W/\"weak\"";   // (no mark to ask by: the same bytes are not promised)
            c.CutAt(100000);
            Check(c.Get() == null && c.Whole && c.Server.Asked.Last().From == null, "a part was gone on from without the server's mark for its file");
            c = new Case().CutAt(100000);
            Check(c.Get(url: Url.Replace("v9.9.9", "v9.9.8")) == null && c.Whole && c.Server.Asked.Last().From == null, "a part begun for another address was gone on from");
            c = new Case().CutAt(100000);
            c.Server.Body = Bytes(310000, 3);   // (the release lists another size now)
            Check(c.Get() == null && c.Whole && c.Server.Asked.Last().From == null, "a part begun for a file of another size was gone on from");
            c = new Case();
            System.IO.File.WriteAllBytes(c.Part, Bytes(5000, 4));   // (an older Setup's part: no note says what it is a part of)
            Check(c.Get() == null && c.Whole && c.Server.Asked.Last().From == null, "a part without its note was gone on from");
            c = new Case().CutAt(100000);
            System.IO.File.WriteAllBytes(c.Part, Bytes(300000, 5));   // (as long as the file: nothing to go on from)
            Check(c.Get() == null && c.Whole && c.Server.Asked.Last().From == null, "a part as long as the file was taken for the file");
            c = new Case();
            c.Server.Cut = 100000;
            Check(c.Get(size: 0) is IOException && c.PartLength == 100000, "(the test) a cut download of a file the release lists no size for");
            c.Server.Cut = -1;
            c.Server.Body = c.Server.Body.Take(60000).ToArray();   // (the server's file is shorter than the part now, under the same mark)
            Check(c.Get(size: 0) == null && c.Whole && c.Server.Asked.Count == 3 && c.Server.Asked[1].From == 100000 && c.Server.Asked[2].From == null,
                  "a part longer than the server's file (416) was not fetched anew, once");
            c = new Case().CutAt(100000);
            c.Server.WrongRange = true;
            Check(c.Get() == null && c.Whole && c.Server.Asked.Count == 3 && c.Server.Asked[2].From == null, "another piece than the one asked for was joined on");

            // checked before it is used
            c = new Case();
            var e = c.Get(size: 300001);
            Check(e is IOException && e is not CutOffException && e.Message.Contains("300000") && e.Message.Contains("300001") && c.NoFile && c.PartLength < 0, "a server that names another size than the release was believed: " + e);
            c = new Case();
            c.Server.Short = 90000;   // (the answer ends early, without an error)
            e = c.Get();
            Check(e is CutOffException && e.Message.Contains("ended early") && c.NoFile && c.PartLength == 210000, "a file that came short got its name: " + e);
            c.Server.Short = 0;
            Check(c.Get() == null && c.Whole && c.Server.Asked.Last().From == 210000, "a download that ended early was not gone on from");
            c = new Case();
            c.Server.Extra = 5000;
            c.Server.NoLength = true;
            e = c.Get();
            Check(e is IOException && e.Message.Contains("more than") && c.NoFile && c.PartLength < 0, "a server that sent more than the file was believed: " + e);
            c = new Case();
            c.Server.NoLength = true;
            Check(c.Get() == null && c.Whole, "a server that names no length: the release's size was not enough to take the file");
            Check(c.Get(size: 0) == null && c.Whole, "neither the release nor the server names a size: what came was not taken");
            c = new Case();
            c.Server.Short = 1;
            Check(c.Get(size: 0) is IOException && c.NoFile, "without a listed size, a file shorter than the server said got its name");
            c = new Case();
            c.Server.Status = HttpStatusCode.NotFound;
            Check(c.Get() is HttpRequestException && c.NoFile && c.PartLength < 0, "a file the server does not have left something behind");

            // Cancel, and a server that goes silent
            c = new Case();
            c.Server.Silent = 50000;
            using (var cancel = new CancellationTokenSource(400))
            {
                var watch = System.Diagnostics.Stopwatch.StartNew();
                e = c.Get(token: cancel.Token);
                Check(e is OperationCanceledException && watch.ElapsedMilliseconds < 10000, "Cancel did not end a download that waits on the network: " + e);
                Check(c.NoFile && c.PartLength == 50000, "a cancelled download did not keep its part");
            }
            c.Server.Silent = -1;
            Check(c.Get() == null && c.Whole && c.Server.Asked.Last().From == 50000, "a cancelled download was not gone on from");
            // (a connection whose read never ends and that takes its time to close: Cancel does not wait for either)
            c = new Case();
            c.Server.Silent = 50000;
            c.Server.Stubborn = true;
            using (var cancel = new CancellationTokenSource(300))
            {
                var watch = System.Diagnostics.Stopwatch.StartNew();
                e = c.Get(token: cancel.Token);
                Check(e is OperationCanceledException && watch.ElapsedMilliseconds < 4000, "Cancel waited for a connection that does not let go (" + watch.ElapsedMilliseconds + " ms): " + e);
                Check(c.NoFile && c.PartLength == 50000, "a download cancelled on such a connection did not keep its part");
            }
            c.Server.Silent = -1;
            c.Server.Stubborn = false;
            Check(c.Get() == null && c.Whole && c.Server.Asked.Last().From == 50000, "a download cancelled on such a connection was not gone on from");
            Download.Stall = TimeSpan.FromMilliseconds(400);
            c = new Case();
            c.Server.Silent = 70000;
            e = c.Get();
            Check(e is CutOffException && e.Message.Contains("nothing more came") && c.PartLength == 70000, "a server that went silent was waited for without end, or not told as that: " + e);
            c.Server.Silent = -1;
            Check(c.Get() == null && c.Whole && c.Server.Asked.Last().From == 70000, "a download that stalled was not gone on from");
            c = new Case();
            c.Server.NeverAnswers = true;
            e = c.Get();
            Check(e is OperationCanceledException && c.NoFile, "a server that never answers was waited for without end: " + e);
            Download.Stall = realStall;

            // room on the disk, for what is still to come
            c = new Case();
            Space.Free = _ => 1000;
            e = c.Get();
            Check(e is NoRoomException && e.Message.Contains("Triforce-Tools-v9.9.9.zip") && c.NoFile && c.PartLength < 0, "a download onto a full disk was begun: " + e);
            Space.Free = realFree;
            c = new Case().CutAt(100000);
            Space.Free = _ => 200000 + Space.Margin - 1;
            Check(c.Get() is NoRoomException && c.PartLength == 100000, "the rest of a download was begun without room for it, or the part went with the refusal");
            Space.Free = _ => 200000 + Space.Margin;
            Check(c.Get() == null && c.Whole, "with room for the rest alone, a download was not gone on from");
            Space.Free = realFree;
        }
        finally
        {
            Space.Free = realFree;
            Download.Stall = realStall;
            try { Directory.Delete(dir, true); } catch (Exception) { }
        }

        Console.WriteLine("setup_download_test: " + checks + " checks passed");
        return 0;
    }
}
