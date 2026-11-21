using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text;
using Triforce.Logic;

namespace Triforce.LogicTests;

sealed class TestFailed : Exception { public TestFailed(string m) : base(m) { } }

static class T
{
    public static void Check(bool ok, string what) { if (!ok) throw new TestFailed(what); }
    public static void Equal<TV>(TV expected, TV actual, string what)
    {
        if (!EqualityComparer<TV>.Default.Equals(expected, actual)) throw new TestFailed(what + ": expected [" + expected + "], got [" + actual + "]");
    }
    public static TE Throws<TE>(Action a, string what) where TE : Exception
    {
        try { a(); }
        catch (TE e) { return e; }
        catch (Exception e) { throw new TestFailed(what + ": threw " + e.GetType().Name + " (" + e.Message + ") instead of " + typeof(TE).Name); }
        throw new TestFailed(what + ": did not throw " + typeof(TE).Name);
    }
    public static async Task<TE> ThrowsAsync<TE>(Func<Task> a, string what) where TE : Exception
    {
        try { await a(); }
        catch (TE e) { return e; }
        catch (Exception e) { throw new TestFailed(what + ": threw " + e.GetType().Name + " (" + e.Message + ") instead of " + typeof(TE).Name); }
        throw new TestFailed(what + ": did not throw " + typeof(TE).Name);
    }

    static readonly string Base = Path.Combine(Path.GetTempPath(), "triforce-logictests-" + Environment.ProcessId);
    static int n;

    /// <summary>A fresh TRIFORCE_TEST_ROOT folder.</summary>
    public static string NewRoot(string name)
    {
        var root = Path.Combine(Base, Interlocked.Increment(ref n).ToString("00") + "-" + name);
        Directory.CreateDirectory(root);
        return root;
    }

    public static void CleanUp()
    {
        try { if (Directory.Exists(Base)) Directory.Delete(Base, true); } catch (Exception) { }
    }

    /// <summary>A context whose test root is root, as if the launcher ran as &lt;root&gt;\Programs\Triforce\TriforceSetup.exe (so
    /// nothing is written beside the test program itself).</summary>
    public static TriforceContext Ctx(string root, string? self = null, Dictionary<string, string?>? env = null)
    {
        var e = new Dictionary<string, string?>(env ?? new()) { ["TRIFORCE_TEST_ROOT"] = root };
        return TriforceContext.Create(e, self ?? Path.Combine(root, "Programs", "Triforce", BuildInfo.SetupAsset));
    }

    /// <summary>This test program's files (its apphost, dlls, runtimeconfig): copied where a fake python or a fake new launcher
    /// must run; exeName names the copy of the apphost.</summary>
    public static void CopySelf(string folder, string exeName)
    {
        Directory.CreateDirectory(folder);
        var dir = AppContext.BaseDirectory;
        File.Copy(Environment.ProcessPath!, Path.Combine(folder, exeName), true);
        foreach (var f in Directory.GetFiles(dir))
        {
            var name = Path.GetFileName(f);
            if (name.EndsWith(".dll") || name.EndsWith(".runtimeconfig.json") || name.EndsWith(".deps.json"))
                File.Copy(f, Path.Combine(folder, name), true);
        }
    }

    /// <summary>The bytes of this program's apphost: served as the new TriforceSetup.exe.</summary>
    public static byte[] SelfBytes => File.ReadAllBytes(Environment.ProcessPath!);
}

/// <summary>A small HTTP/1.1 server on 127.0.0.1 for the release check and the downloads: GET only, a strong ETag, Range
/// ("bytes=N-") answered with 206 and If-Range honoured, 404 for anything it does not have. (TcpListener, not HttpListener: no URL
/// reservation is needed for a user without admin rights.)</summary>
sealed class FakeServer : IDisposable
{
    readonly TcpListener listener;
    readonly Dictionary<string, (byte[] Body, string Type, int Status)> routes = new();
    readonly CancellationTokenSource stop = new();
    public int Requests;
    public readonly List<string> Seen = new();

    public FakeServer()
    {
        listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        _ = Task.Run(Loop);
    }

    public string Url => "http://127.0.0.1:" + ((IPEndPoint)listener.LocalEndpoint).Port;

    public void Add(string path, byte[] body, string type = "application/octet-stream", int status = 200)
    {
        lock (routes) routes[path] = (body, type, status);
    }
    public void Add(string path, string text, string type = "application/json", int status = 200) => Add(path, Encoding.UTF8.GetBytes(text), type, status);

    async Task Loop()
    {
        while (!stop.IsCancellationRequested)
        {
            TcpClient c;
            try { c = await listener.AcceptTcpClientAsync(stop.Token); } catch (Exception) { return; }
            _ = Task.Run(() => Serve(c));
        }
    }

    async Task Serve(TcpClient c)
    {
        using (c)
        try
        {
            var s = c.GetStream();
            var head = new StringBuilder();
            var buf = new byte[1];
            while (!head.ToString().EndsWith("\r\n\r\n"))
            {
                if (await s.ReadAsync(buf) == 0) return;
                head.Append((char)buf[0]);
            }
            var lines = head.ToString().Split("\r\n");
            var path = lines[0].Split(' ')[1];
            Interlocked.Increment(ref Requests);
            lock (Seen) Seen.Add(path);
            long from = -1;
            string? ifRange = null;
            foreach (var l in lines.Skip(1))
            {
                if (l.StartsWith("Range: bytes=", StringComparison.OrdinalIgnoreCase)) from = long.Parse(l.Substring(13).TrimEnd('-'));
                if (l.StartsWith("If-Range: ", StringComparison.OrdinalIgnoreCase)) ifRange = l.Substring(10);
            }
            (byte[] Body, string Type, int Status) r;
            bool found;
            lock (routes) found = routes.TryGetValue(path, out r);
            if (!found) r = (Encoding.UTF8.GetBytes("not found"), "text/plain", 404);
            var etag = "\"v" + r.Body.Length + "\"";
            var body = r.Body;
            var status = r.Status;
            var extra = "ETag: " + etag + "\r\n";
            if (from > 0 && status == 200 && (ifRange == null || ifRange == etag))
            {
                if (from >= body.Length) { status = 416; body = Array.Empty<byte>(); }
                else
                {
                    extra += "Content-Range: bytes " + from + "-" + (r.Body.Length - 1) + "/" + r.Body.Length + "\r\n";
                    body = r.Body.AsSpan((int)from).ToArray();
                    status = 206;
                }
            }
            var text = status switch { 200 => "OK", 206 => "Partial Content", 404 => "Not Found", 416 => "Range Not Satisfiable", _ => "Status" };
            var header = "HTTP/1.1 " + status + " " + text + "\r\nContent-Type: " + r.Type + "\r\nContent-Length: " + body.Length + "\r\n" + extra + "Connection: close\r\n\r\n";
            await s.WriteAsync(Encoding.ASCII.GetBytes(header));
            await s.WriteAsync(body);
            await s.FlushAsync();
        }
        catch (Exception) { }
    }

    public void Dispose()
    {
        stop.Cancel();
        listener.Stop();
    }
}

/// <summary>The fakes this program plays when a test starts it as another program.</summary>
static class Fakes
{
    /// <summary>A stand-in game for the benchmark (copied in as Triforce.exe: "image.bin --romfs .. --user .."): the title's fps
    /// lines, a save that loads after three A presses, "!cheat 1000+n" teleports, and per place fps / 1% low lines; each 5 s
    /// window of the real game is TRIFORCE_FAKE_WINDOW_MS here (default 400). Frame rates: by place, x1.4 with an upscaler on in
    /// the options file beside the exe, and the cap there. It writes a save into --user, to show the benchmark's copy takes it.</summary>
    /// <summary>The stand-in game's benchmark mode: "bench:" progress lines, then the results file (as host/ writes it), then
    /// it ends. Frame rates by scene, x1.4 with an upscaler on in the options file beside the exe. TRIFORCE_FAKE_WINDOW_MS paces it.</summary>
    static int FakeBenchmark(string results, string? user)
    {
        var ic = System.Globalization.CultureInfo.InvariantCulture;
        var win = int.TryParse(Environment.GetEnvironmentVariable("TRIFORCE_FAKE_WINDOW_MS"), out var w) ? w : 400;
        var opts = Triforce.Logic.BenchIni.Read(Path.Combine(AppContext.BaseDirectory, "triforce_options.ini"));
        var up = opts.TryGetValue("dlss", out var u) && u != 0 ? 1.4 : 1.0;
        Console.Error.WriteLine("bench: Loading your newest save"); Thread.Sleep(win * 3);
        if (user != null) { Directory.CreateDirectory(Path.Combine(user, "save")); File.WriteAllText(Path.Combine(user, "save", "written-by-the-game.txt"), "x"); }
        var scenes = new[] { ("Hyrule Field", 92.0), ("Castle Town Market", 71.0), ("Kokiri Forest", 118.0), ("Lake Hylia", 133.0) };
        var json = new System.Text.StringBuilder("{\"version\":1,\"stopped\":false,\"gpu\":\"Stand-in GPU\",\"scenes\":[");
        for (var i = 0; i < scenes.Length; i++)
        {
            var (name, fps) = scenes[i];
            Console.Error.WriteLine($"bench: Going to {name}"); Thread.Sleep(win);
            Console.Error.WriteLine($"bench: Scene {i + 1} of {scenes.Length} · {name}"); Thread.Sleep(win * 3);
            var f = fps * up; var ft = Enumerable.Range(0, 120).Select(k => 1000 / f * (1 + 0.08 * Math.Sin(k / 5.0) + (k % 37 == 0 ? 0.6 : 0)));
            json.Append(string.Create(ic, $"{(i > 0 ? "," : "")}{{\"name\":\"{name}\",\"avg\":{f:0.0},\"low1\":{f * 0.84:0.0},\"low01\":{f * 0.7:0.0},\"frames\":{(int)(f * 15)},\"seconds\":15,\"frametimes_ms\":[{string.Join(",", ft.Select(x => x.ToString("0.00", ic)))}]}}"));
        }
        json.Append("]}");
        File.WriteAllText(results + ".new", json.ToString()); File.Move(results + ".new", results, true);
        Console.Error.WriteLine("bench: Finished"); Thread.Sleep(win);
        return 0;
    }

    public static int FakeGame(string[] a)
    {
        string? Arg(string name) { var i = Array.IndexOf(a, name); return i >= 0 && i + 1 < a.Length ? a[i + 1] : null; }
        if (Environment.GetEnvironmentVariable("HOS_BENCHMARK") is { Length: > 0 } results) return FakeBenchmark(results, Arg("--user"));
        var input = Environment.GetEnvironmentVariable("HOS_INPUT_LIVE");
        var win = int.TryParse(Environment.GetEnvironmentVariable("TRIFORCE_FAKE_WINDOW_MS"), out var w) ? w : 400;
        var ini = Path.Combine(AppContext.BaseDirectory, "triforce_options.ini");
        var opts = Triforce.Logic.BenchIni.Read(ini);
        var up = opts.TryGetValue("dlss", out var u) && u != 0 ? 1.4 : 1.0;
        var capFps = (opts.TryGetValue("frame_rate_cap", out var c) ? c : 0) switch { 0 => 60, 1 => 90, 2 => 120, 3 => 144, 4 => 165, _ => 240 };
        var place = new Dictionary<int, double> { [1000] = 118, [1001] = 92, [1003] = 71, [1007] = 133 };
        double baseFps = 80; var presses = 0; var loaded = false; long frames = 0; int readAt = 0;
        var user = Arg("--user");
        for (var t = 0; t < 100000; t++)
        {
            Thread.Sleep(win);
            if (input != null && File.Exists(input))
            {
                var all = File.ReadAllText(input);
                foreach (var line in all[readAt..].Split('\n', StringSplitOptions.RemoveEmptyEntries))
                {
                    if (line.StartsWith("!cheat "))
                    {
                        var id = int.Parse(line[7..].Trim());
                        Console.Error.WriteLine($"cheats: teleport ... to {id} at ...: request sent, result 0x0");
                        if (place.TryGetValue(id, out var f)) baseFps = f;
                    }
                    else presses += line.Split(',').Count(x => x.EndsWith(":A"));
                }
                readAt = all.Length;
            }
            if (!loaded && presses >= 3) { loaded = true; if (user != null) { Directory.CreateDirectory(Path.Combine(user, "save")); File.WriteAllText(Path.Combine(user, "save", "written-by-the-game.txt"), "x"); } }
            var fps = Math.Min(capFps, (loaded ? baseFps * up : 60) + Math.Sin(t) * 3);
            frames += (long)(fps * 5);
            var ic = System.Globalization.CultureInfo.InvariantCulture;
            Console.Error.WriteLine(string.Create(ic, $"vi: {frames} frames presented ({fps:0.0} fps over the last 5.0 s, interval 1)"));
            Console.Error.WriteLine(string.Create(ic, $"vi: 1% low {fps * 0.86:0.0} fps over the last 5.0 s ({(int)(fps * 5)} frames)"));
            if (loaded) Console.Error.WriteLine(string.Create(ic, $"vk: {frames} presents, {fps:0.0} fps; a frame: 2600 draws"));
            Console.Error.Flush();
        }
        return 0;
    }

    /// <summary>make_game.py's lines as a build prints them (formats from nx2/make_game.py say(), build.py's @@plan/@@file/@@eta, and
    /// the game's own "@@stage shaders" from gpu/vk/vk_main.cpp), {OUT} the game folder.</summary>
    public const string Transcript = """
        the Windows SDK is here: C:\Program Files (x86)\Windows Kits\10
        @@stage read Reading your game files
        @@progress 0.000
        @@progress 0.500
        @@progress 1.000
        @@stage lift Translating the game to PC code
        lift: 30211 functions
        @@progress 0.300
        @@stage compile Compiling
        @@plan 12 579
        @@file code_hot_0.c 41.2 new
        @@file code_1.c 0.0 cached
        @@file code_2.c 3.5 again
        @@eta 300
        @@progress 0.400
        compiled in 790s
        @@stage finish Putting the game folder together
        the build took 0:41:00, compiled in 0:30:00 (the last one 0:45:00)
        @@times 2460 1800 2700
        the build's steps: before the game files 0:41, read 1:32, lift 2:10, compile 7:42, finish 1:17
        @@stage shaders Preparing shaders
        @@progress 0.500
        @@progress 1.000
        @@done {OUT}
        """;

    /// <summary>"python -I make_game.py ...": --check answers ok; a build plays the transcript and makes the folder (Triforce.exe
    /// and triforce_version.txt), "@@error" for an --out with FAIL in it, and sits in its first stage for one with SLOW in it.</summary>
    public static int MakeGame(string[] a)
    {
        string? Arg(string name) { var i = Array.IndexOf(a, name); return i >= 0 && i + 1 < a.Length ? a[i + 1] : null; }
        if (a.Contains("--check"))
        {
            Console.WriteLine("@@check ok This is the version Triforce supports (build e0f3ccfa).");
            return 0;
        }
        var outDir = Arg("--out") ?? Arg("--update") ?? "out";
        File.WriteAllText(Path.Combine(Path.GetDirectoryName(Arg("--work") ?? ".")!, "fake-make-game-args.txt"), string.Join("\n", a));
        foreach (var raw in Transcript.Split('\n'))
        {
            var line = raw.TrimEnd('\r').Replace("{OUT}", outDir);
            if (line.StartsWith("@@done"))
            {
                Directory.CreateDirectory(outDir);
                File.WriteAllText(Path.Combine(outDir, "Triforce.exe"), "fake game");
                File.WriteAllText(Path.Combine(outDir, "triforce_version.txt"), "version=v9.9.9\nbuilder=" + Environment.GetEnvironmentVariable("HOOKSHOT_BUILDER_EXE") + "\n");
            }
            Console.WriteLine(line);
            Console.Out.Flush();
            Thread.Sleep(int.TryParse(Environment.GetEnvironmentVariable("TRIFORCE_FAKE_LINE_MS"), out var ms) ? ms : 15);
            if (line.StartsWith("@@stage read") && outDir.Contains("SLOW")) { Thread.Sleep(60000); return 9; }
            if (line.StartsWith("@@stage compile") && outDir.Contains("FAIL"))
            {
                Console.Error.WriteLine("clang: error: no space left on device");
                Console.WriteLine("@@error The disk is full.");
                return 1;
            }
        }
        return 0;
    }

    /// <summary>A new TriforceSetup.exe started by a self-update: "up" gives the sign its window is up and ends well, "fail" ends with
    /// an error before it. Its arguments go to &lt;root&gt;\child-args.txt.</summary>
    public static int NewSetup(string mode, string[] args)
    {
        var root = Environment.GetEnvironmentVariable("TRIFORCE_TEST_ROOT")!;
        File.WriteAllText(Path.Combine(root, "child-args.txt"), string.Join("|", args));
        if (mode == "fail") return 3;
        var ctx = TriforceContext.Create();
        Startup.SignalUp(ctx);
        Thread.Sleep(300);
        return 0;
    }
}
