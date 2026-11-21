using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using Triforce.Logic;
using static Triforce.LogicTests.T;

namespace Triforce.LogicTests;

static partial class Tests
{
    const string Notes = "Triforce v0.5.2 brings these:\n\n**Faster updates.** The tools come in two parts now.\n* **Mods page.** GameBanana's mods in the launcher, with `kits`.\n" +
                         "**A new look.** The launcher is [rebuilt](https://example.invalid).\n**A fourth one.** Not shown.\n\n## Fixes\n* one\n";

    /// <summary>A Forgejo answer: a list of one release whose assets each carry a nested uploader object.</summary>
    static string ReleaseJson(string server, string tag, IEnumerable<(string Name, long Size)> assets, string body = Notes)
    {
        var list = string.Join(",", assets.Select((a, i) =>
            "{\"id\":" + (100 + i) + ",\"name\":\"" + a.Name + "\",\"size\":" + a.Size + ",\"download_count\":3,\"uploader\":{\"id\":1,\"login\":\"triforce\",\"assets\":[]}," +
            "\"browser_download_url\":\"" + server + "/assets/" + a.Name + "\"}"));
        return "[{\"id\":17,\"tag_name\":\"" + tag + "\",\"name\":\"Triforce " + tag + "\",\"prerelease\":true,\"html_url\":\"" + server + "/release\",\"published_at\":\"2026-11-19T10:00:00Z\"," +
               "\"body\":" + System.Text.Json.JsonSerializer.Serialize(body) + ",\"assets\":[" + list + "]}]";
    }

    static async Task ReleaseCheck()
    {
        using var s = new FakeServer();
        var root = NewRoot("release");
        var setup = Encoding.ASCII.GetBytes("new setup");
        s.Add("/api/releases", ReleaseJson(s.Url, "v0.5.2", new[] { ("TriforceSetup.exe", (long)setup.Length), ("triforce-tools.ZIP", 1234L), ("Triforce-Tools-code.zip", 12L),
            ("Triforce-Tools-base.zip", 34L), ("Triforce-v0.5.2-linux-test-x86_64.AppImage", 99L), ("other.txt", 1L) }));
        var ctx = Ctx(root, env: new() { ["TRIFORCE_RELEASE_API"] = s.Url + "/api/releases" });
        using var http = Downloader.CreateHttp();
        var r = await new Updater(ctx).CheckAsync(http, CancellationToken.None);
        Equal("v0.5.2", r.Tag, "tag");
        Equal(s.Url + "/assets/TriforceSetup.exe", r.SetupUrl, "setup url");
        Equal((long)setup.Length, r.SetupSize, "setup size");
        Equal(1234L, r.ToolsSize, "tools matched without case");
        Check(r.ToolsCodeUrl != null && r.ToolsBaseSize == 34, "parts");
        Check(r.LinuxAppImageUrl != null && r.LinuxListUrl == null, "the AppImage; no linux list (none published)");
        Check(r.Prerelease && r.PageUrl == s.Url + "/release", "page and prerelease");
        var newIn = r.NewIn;
        Equal(3, newIn.Count, "three New in items");
        Equal("Faster updates", newIn[0].Title, "title without its dot");
        Equal("The tools come in two parts now.", newIn[0].Text, "its line");
        Equal("GameBanana's mods in the launcher, with kits.", newIn[1].Text, "markdown out");
        Equal("The launcher is rebuilt.", newIn[2].Text, "link text kept");
        var bullets = ReleaseNotes.NewIn("Intro\n\n* First fix\n* **Second.** two\n- third\n- fourth");
        Equal("First fix|Second|third", string.Join("|", bullets.Select(b => b.Title)), "fallback: the first bullets");
        // (404: no release yet)
        var none = Ctx(root, env: new() { ["TRIFORCE_RELEASE_API"] = s.Url + "/missing" });
        await ThrowsAsync<NoReleaseException>(() => Release.LatestAsync(none, http, CancellationToken.None), "404");
        s.Add("/empty", "[]");
        var empty = Ctx(root, env: new() { ["TRIFORCE_RELEASE_API"] = s.Url + "/empty" });
        await ThrowsAsync<NoReleaseException>(() => Release.LatestAsync(empty, http, CancellationToken.None), "an empty list");
        var single = Release.Parse("{\"tag_name\":\"v1\",\"assets\":[]}");
        Equal("v1", single.Tag, "a single release object");
    }

    static async Task DownloadResume()
    {
        using var s = new FakeServer();
        var root = NewRoot("download");
        var ctx = Ctx(root);
        var data = RandomNumberGenerator.GetBytes(3_000_000);
        s.Add("/assets/big.bin", data);
        using var http = Downloader.CreateHttp();
        var file = Path.Combine(root, "downloads", "big.bin");
        var seen = new List<DownloadProgress>();
        await new Downloader(ctx).ToFileAsync(http, s.Url + "/assets/big.bin", file, data.Length, p => seen.Add(p), CancellationToken.None);
        Check(File.ReadAllBytes(file).SequenceEqual(data), "the file whole");
        Check(seen.Count > 0 && seen[^1].Fraction == 1, "progress to 1");
        Check(!File.Exists(file + ".part") && !File.Exists(file + ".part.of"), "no part left");
        // (the size the release lists is not what the server sends: refused before a byte is kept)
        var bad = Path.Combine(root, "downloads", "bad.bin");
        await ThrowsAsync<IOException>(() => new Downloader(ctx).ToFileAsync(http, s.Url + "/assets/big.bin", bad, data.Length + 5, null, CancellationToken.None), "a size that contradicts the release");
        Check(!File.Exists(bad), "nothing kept");
        // (a part from a cut download goes on from where it stopped: Range + If-Range)
        var resumed = Path.Combine(root, "downloads", "resumed.bin");
        File.WriteAllBytes(resumed + ".part", data.AsSpan(0, 1_000_000).ToArray());
        File.WriteAllLines(resumed + ".part.of", new[] { s.Url + "/assets/big.bin", data.Length.ToString(), "\"v" + data.Length + "\"" });
        long first = -1;
        await new Downloader(ctx).ToFileAsync(http, s.Url + "/assets/big.bin", resumed, data.Length, p => { if (first < 0) first = p.Bytes; }, CancellationToken.None);
        Check(File.ReadAllBytes(resumed).SequenceEqual(data), "resumed file whole");
        Equal(1_000_000L, first, "it went on from the part");
        // (a part whose note names another address is fetched anew)
        var other = Path.Combine(root, "downloads", "other.bin");
        File.WriteAllBytes(other + ".part", new byte[] { 1, 2, 3 });
        File.WriteAllLines(other + ".part.of", new[] { "https://elsewhere.invalid/x", data.Length.ToString(), "\"x\"" });
        await new Downloader(ctx).ToFileAsync(http, s.Url + "/assets/big.bin", other, data.Length, null, CancellationToken.None);
        Check(File.ReadAllBytes(other).SequenceEqual(data), "a foreign part was not joined on");
    }

    /// <summary>An installed launcher in a test root that can run: this program's files beside a TriforceSetup.exe that says "OLD".</summary>
    static (TriforceContext Ctx, string Dir, string Root) InstalledRoot(string name)
    {
        var root = NewRoot(name);
        var dir = Path.Combine(root, "Programs", "Triforce");
        CopySelf(dir, "copy-of-tests.exe");
        File.WriteAllText(Path.Combine(dir, BuildInfo.SetupAsset), "OLD");
        var ctx = Ctx(root, Path.Combine(dir, BuildInfo.SetupAsset));
        ctx.Platform.Settings.SetUninstall("InstallLocation", dir);
        return (ctx, dir, root);
    }

    static async Task<Release> ServeNewSetup(FakeServer s, string tag)
    {
        var bytes = SelfBytes;
        s.Add("/assets/TriforceSetup.exe", bytes);
        s.Add("/api", ReleaseJson(s.Url, tag, new[] { ("TriforceSetup.exe", (long)bytes.Length) }));
        await Task.CompletedTask;
        return Release.Parse(ReleaseJson(s.Url, tag, new[] { ("TriforceSetup.exe", (long)bytes.Length) }));
    }

    static async Task SelfSwapUp()
    {
        using var s = new FakeServer();
        var (ctx, dir, root) = InstalledRoot("selfswap-up");
        var r = await ServeNewSetup(s, "v9.0.0");
        var held = InstanceLock.TryTake(dir, root)!;
        Environment.SetEnvironmentVariable("TRIFORCE_TEST_ROOT", root);
        Environment.SetEnvironmentVariable("LOGICTESTS_CHILD", "up");
        var hidden = false;
        try
        {
            using var http = Downloader.CreateHttp();
            var u = new Updater(ctx) { HandoverLimit = TimeSpan.FromSeconds(20) };
            var result = await u.SelfUpdateAsync(http, r, new StartArgs(), held, null, () => hidden = true, CancellationToken.None);
            Equal(SelfUpdateResult.CloseNow, result, "the old one closes");
            Check(hidden, "the window was hidden while it waited");
            Check(File.ReadAllBytes(Path.Combine(dir, BuildInfo.SetupAsset)).SequenceEqual(SelfBytes), "the new exe under the name");
            Equal("OLD", File.ReadAllText(Path.Combine(dir, "TriforceSetup.old.exe")), "the running one stepped aside");
            Check(!File.Exists(Path.Combine(dir, "TriforceSetup.new.exe")), "no .new left");
            Equal("--relaunched|--merge", File.ReadAllText(Path.Combine(root, "child-args.txt")), "started as an old Setup starts it");
            Check(ctx.Log.ReadAll().Contains("its window is up"), "it gave its sign: " + ctx.Log.ReadAll());
            Equal(null, ctx.Settings.SetupSkip, "nothing skipped");
        }
        finally
        {
            Environment.SetEnvironmentVariable("LOGICTESTS_CHILD", null);
            Environment.SetEnvironmentVariable("TRIFORCE_TEST_ROOT", null);
            held.Dispose();
        }
    }

    static async Task SelfSwapFails()
    {
        using var s = new FakeServer();
        var (ctx, dir, root) = InstalledRoot("selfswap-fail");
        var r = await ServeNewSetup(s, "v9.0.1");
        var held = InstanceLock.TryTake(dir, root)!;
        Environment.SetEnvironmentVariable("TRIFORCE_TEST_ROOT", root);
        Environment.SetEnvironmentVariable("LOGICTESTS_CHILD", "fail");
        try
        {
            using var http = Downloader.CreateHttp();
            var u = new Updater(ctx) { HandoverLimit = TimeSpan.FromSeconds(20) };
            var e = await ThrowsAsync<NotStartedException>(() => u.SelfUpdateAsync(http, r, new StartArgs { UpdateFolder = @"C:\G", Merge = true }, held, null, null, CancellationToken.None), "a new launcher that fails");
            Check(e.Message.Contains("is back in its place"), e.Message);
            Equal("OLD", File.ReadAllText(Path.Combine(dir, BuildInfo.SetupAsset)), "the old exe back under the name");
            Equal("v9.0.1", ctx.Settings.SetupSkip, "that tag is skipped from now on");
            Check(held.Held, "the lock taken again");
            Equal("--update|C:\\G|--relaunched", File.ReadAllText(Path.Combine(root, "child-args.txt")), "the game's folder passed on");
            Check(!new Updater(ctx).LauncherNewer(r), "a skipped (or dev) launcher is not newer");
        }
        finally
        {
            Environment.SetEnvironmentVariable("LOGICTESTS_CHILD", null);
            Environment.SetEnvironmentVariable("TRIFORCE_TEST_ROOT", null);
            held.Dispose();
        }
    }

    static void Decide()
    {
        var root = NewRoot("decide");
        var ctx = Ctx(root);
        var u = new Updater(ctx);
        var r = new Release { Tag = "v0.5.2", SetupUrl = "x", ToolsUrl = "y" };
        Equal(CheckOutcome.Merge, u.Decide(new StartArgs(), r, null).Outcome, "first start, nothing here: the tools come at once");
        Equal(CheckOutcome.Failed, u.Decide(new StartArgs(), null, new HttpRequestException("x")).Outcome, "offline with nothing");
        Equal(FailureKind.Offline, u.Decide(new StartArgs(), null, new HttpRequestException("x")).Failure!.Kind, "said as offline");
        Equal(FailureKind.Cancelled, u.Decide(new StartArgs(), null, new OperationCanceledException(), cancelled: true).Failure!.Kind, "cancelled");
        // (tools here)
        Directory.CreateDirectory(Path.Combine(ctx.ToolsFolder, "nx2"));
        File.WriteAllText(Path.Combine(ctx.ToolsFolder, "nx2", "make_game.py"), "");
        File.WriteAllText(Path.Combine(ctx.ToolsFolder, "version.txt"), "v0.5.1\n");
        Equal(CheckOutcome.Home, u.Decide(new StartArgs(), r, null).Outcome, "with tools: Home");
        Equal(CheckOutcome.Merge, u.Decide(new StartArgs { Merge = true }, r, null).Outcome, "--merge: carries on");
        Equal(CheckOutcome.HomeOffline, u.Decide(new StartArgs(), null, new HttpRequestException("x")).Outcome, "offline with tools: Home");
        Check(u.ToolsNewer(r), "tools v0.5.1 behind v0.5.2");
        var game = Path.Combine(root, "Games", "Triforce");
        Directory.CreateDirectory(game);
        File.WriteAllText(Path.Combine(game, "Triforce.exe"), "");
        File.WriteAllText(Path.Combine(game, "triforce_version.txt"), "version=v0.5.0\n");
        ctx.Settings.GameFolder = game;
        var st = u.State(r);
        Check(st.ToolsNewer && st.GameBehind && !st.LauncherNewer && st.Any, "home's update state (a dev launcher never replaces itself)");
        var plan = u.Plan(new StartArgs { Merge = true }, r);
        Check(plan.Tools && plan.RebuildFolder == game && !plan.SelfUpdate, "merge plan: tools, then the game rebuilt");
    }

    // ---- tools ----

    static byte[] Zip(params (string Name, string Text)[] files)
    {
        using var m = new MemoryStream();
        using (var z = new ZipArchive(m, ZipArchiveMode.Create, true))
            foreach (var (name, text) in files)
            {
                var e = z.CreateEntry(name);
                if (!name.EndsWith('/')) using (var w = new StreamWriter(e.Open())) w.Write(text);
            }
        return m.ToArray();
    }

    static string Sha(string text) => Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(text)));

    static string BaseList(params (string Path, string Text)[] files) =>
        "triforce-tools-base\t1\nid\t" + Sha("base") + "\ndir\tpython\n" + string.Concat(files.Select(f => "file\t" + Encoding.UTF8.GetByteCount(f.Text) + "\t" + Sha(f.Text) + "\t" + f.Path + "\n"));

    static void ToolsUnpack()
    {
        var root = NewRoot("tools");
        var ctx = Ctx(root);
        var tools = new Tools(ctx);
        var zip = Path.Combine(root, "t.zip");
        File.WriteAllBytes(zip, Zip(("tools/nx2/make_game.py", "print('x')"), ("tools/python/python.exe", "py"), ("tools/scripts/", ""),
            ("tools/base.list", BaseList(("python/python.exe", "py")))));
        Directory.CreateDirectory(tools.Folder + ".part");   // (left by a cut-off unpacking)
        tools.Sweep();
        Check(!Directory.Exists(tools.Folder + ".part"), "sweep");
        double last = -1;
        tools.Unpack(zip, "v0.5.2", f => last = f);
        Equal("v0.5.2", tools.InstalledVersion, "version.txt written");
        Equal(1.0, last, "progress to 1");
        Check(File.Exists(Path.Combine(tools.Folder, "nx2", "make_game.py")) && Directory.Exists(Path.Combine(tools.Folder, "scripts")), "the prefix tools/ stripped");
        Equal(Sha("base"), File.ReadAllText(Path.Combine(tools.Folder, Tools.BaseHave)).Trim(), "base.have from the whole zip's list");
        var found = tools.Find()!;
        Equal(Path.Combine(tools.Folder, "python", "python.exe"), found.Python, "its own python");
        // (unsafe names: the whole zip refused, the tools there kept)
        foreach (var bad in new[] { "tools/../evil.txt", "tools/CON.txt", "tools/a:b", "tools/name. " })
        {
            File.WriteAllBytes(zip, Zip(("tools/nx2/make_game.py", "x"), (bad, "x")));
            Throws<InvalidDataException>(() => tools.Unpack(zip, "v9", null), "refused: " + bad);
            Equal("v0.5.2", tools.InstalledVersion, "the tools there kept after " + bad);
        }
        // (no tools in a test root without its own: never the development checkout above the test program)
        Equal(null, new Tools(Ctx(NewRoot("notools"))).Find(), "no dev-checkout fallback in a test run");
    }

    static void ToolsParts()
    {
        Equal(null, Tools.ParseBaseList("triforce-tools-base\t1\nid\tabc\n"), "a short id is no list");
        Equal(null, Tools.ParseBaseList("triforce-tools-base\t1\nid\t" + Sha("x") + "\ndir\tpython\nfile\t1\t" + Sha("a") + "\ta\nwhat\tnow\n"), "an unknown line");
        var root = NewRoot("parts");
        var ctx = Ctx(root);
        var tools = new Tools(ctx);
        var list = BaseList(("python/python.exe", "py"), ("python/lib.txt", "library"));
        var b = Tools.ParseBaseList(list)!;
        Equal(2, b.Files.Count, "two base files");
        // (tools here with that base)
        Directory.CreateDirectory(Path.Combine(tools.Folder, "python"));
        Directory.CreateDirectory(Path.Combine(tools.Folder, "nx2"));
        File.WriteAllText(Path.Combine(tools.Folder, "python", "python.exe"), "py");
        File.WriteAllText(Path.Combine(tools.Folder, "python", "lib.txt"), "library");
        File.WriteAllText(Path.Combine(tools.Folder, "nx2", "make_game.py"), "old");
        File.WriteAllText(Path.Combine(tools.Folder, "version.txt"), "v0.5.1");
        Check(Tools.HasBase(b, tools.Folder, null), "the base is here (sha read)");
        Equal(b.Id, File.ReadAllText(Path.Combine(tools.Folder, Tools.BaseHave)).Trim(), "base.have remembered");
        var code = Path.Combine(root, "code.zip");
        File.WriteAllBytes(code, Zip(("tools/nx2/make_game.py", "new"), ("tools/base.list", list)));
        Equal(b.Id, Tools.BaseListIn(code)!.Id, "the code zip's list");
        tools.UnpackParts(code, null, b, "v0.5.2", null);
        Equal("v0.5.2", tools.InstalledVersion, "new version");
        Equal("new", File.ReadAllText(Path.Combine(tools.Folder, "nx2", "make_game.py")), "new code");
        Equal("library", File.ReadAllText(Path.Combine(tools.Folder, "python", "lib.txt")), "the base moved over");
        File.WriteAllText(Path.Combine(tools.Folder, "python", "lib.txt"), "changed!");
        Check(!Tools.HasBase(b, tools.Folder, null), "a changed base file (other size) is not the base");
    }

    static async Task ToolsUpdate()
    {
        using var s = new FakeServer();
        var root = NewRoot("toolsupdate");
        var zip = Zip(("tools/nx2/make_game.py", "print('x')"), ("tools/python/python.exe", "py"));
        s.Add("/assets/Triforce-Tools.zip", zip);
        var r = Release.Parse(ReleaseJson(s.Url, "v0.5.2", new[] { ("Triforce-Tools.zip", (long)zip.Length) }));
        var ctx = Ctx(root);
        var inst = new ToolsInstaller(ctx);
        var steps = new List<ToolsStep>();
        inst.Progress += p => steps.Add(p.Step);
        Check(inst.Needed(r), "no tools: needed");
        using var http = Downloader.CreateHttp();
        await inst.UpdateAsync(http, r, CancellationToken.None);
        Equal("v0.5.2", new Tools(ctx).InstalledVersion, "tools installed");
        Check(steps.Contains(ToolsStep.Download) && steps.Contains(ToolsStep.Unpack) && steps[^1] == ToolsStep.Done, "progress steps: " + string.Join(",", steps));
        Check(Directory.GetFiles(ctx.DownloadsFolder).Length == 0, "the download cache emptied");
        Check(!inst.Needed(r), "up to date now");
        var n = s.Requests;
        await inst.UpdateAsync(http, r, CancellationToken.None);
        Equal(n, s.Requests, "nothing fetched when up to date");
    }
}
