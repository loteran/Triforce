// The Triforce tools beside the installed launcher: nx2/make_game.py and its own Python (Windows: <install>\tools from
// Triforce-Tools.zip; Linux: the AppImage's usr/share/hookshot, HOOKSHOT_TOOLS). Old Setup.cs Tools and Shell.cs Merge step 2.
using System.IO.Compression;
using System.Security.Cryptography;

namespace Triforce.Logic;

/// <summary>Where make_game.py is and the Python that runs it.</summary>
public sealed record ToolSet(string Root, string Python)
{
    public string MakeGame => Path.Combine(Root, "nx2", "make_game.py");
}

/// <summary>A step of a tools update: what it does now and how far it is.</summary>
public sealed record ToolsProgress(ToolsStep Step, double Fraction, long Bytes, long Total, string Text);

public enum ToolsStep { Download, CheckBase, DownloadBase, Unpack, Done }

public sealed class Tools
{
    readonly TriforceContext ctx;
    public Tools(TriforceContext ctx) { this.ctx = ctx; }

    /// <summary>&lt;install&gt;\tools (tools.part and tools.old beside it during a swap).</summary>
    public string Folder => ctx.ToolsFolder;

    /// <summary>The release these tools came from (tools\version.txt, Linux usr/share/hookshot/VERSION), null when there are none.</summary>
    public string? InstalledVersion
    {
        get
        {
            var v = Path.Combine(Folder, "version.txt");
            if (File.Exists(v) && File.Exists(Path.Combine(Folder, "nx2", "make_game.py"))) return ReadLine(v);
            if (!ctx.IsWindows && LinuxTools() is { } t)
                foreach (var name in new[] { "VERSION", "version.txt" })
                    if (ReadLine(Path.Combine(t.Root, name)) is { Length: > 0 } tag) return tag;
            return null;
        }
    }

    static string? ReadLine(string file)
    {
        try { return File.Exists(file) ? File.ReadLines(file).FirstOrDefault()?.Trim() : null; } catch (Exception) { return null; }
    }

    /// <summary>The tools to run make_game with, null when there are none. Windows: &lt;install&gt;\tools with its python\python.exe (or
    /// "python" from the PATH when that file is missing); else, outside a test run, a development checkout above the exe
    /// (TRIFORCE_PYTHON or "python"). Linux: HOOKSHOT_TOOLS, with HOOKSHOT_PYTHON or the image's usr/python/bin/python3.</summary>
    public ToolSet? Find()
    {
        var bundled = Folder;
        if (File.Exists(Path.Combine(bundled, "nx2", "make_game.py")))
        {
            var py = ctx.IsWindows ? Path.Combine(bundled, "python", "python.exe") : Path.Combine(bundled, "python", "bin", "python3");
            return new ToolSet(bundled, File.Exists(py) ? py : ctx.IsWindows ? "python" : "python3");
        }
        if (!ctx.IsWindows && LinuxTools() is { } lt) return lt;
        // (a development copy: the launcher run from inside a checkout uses its sources and the PC's Python. Never in a test run,
        // which has its own tools or none)
        if (ctx.TestRoot != null) return null;
        for (var d = new DirectoryInfo(ctx.SelfDir); d != null; d = d.Parent)
            if (File.Exists(Path.Combine(d.FullName, "nx2", "make_game.py")))
                return new ToolSet(d.FullName, ctx.Env("TRIFORCE_PYTHON") ?? (ctx.IsWindows ? "python" : "python3"));
        return null;
    }

    /// <summary>Linux: the AppImage's tools (AppRun exports HOOKSHOT_TOOLS; research/12 1.3). Its Python: HOOKSHOT_PYTHON when
    /// AppRun exports it (research/12 4.1 asks for that), else &lt;root&gt;/usr/python/bin/python3 beside usr/share/hookshot.</summary>
    ToolSet? LinuxTools()
    {
        var tools = ctx.Env("HOOKSHOT_TOOLS");
        if (string.IsNullOrEmpty(tools) && ctx.Env("APPDIR") is { Length: > 0 } appdir) tools = Path.Combine(appdir, "usr", "share", "hookshot");
        if (string.IsNullOrEmpty(tools) || !File.Exists(Path.Combine(tools, "nx2", "make_game.py"))) return null;
        var py = ctx.Env("HOOKSHOT_PYTHON");
        if (string.IsNullOrEmpty(py))
        {
            var root = Path.GetDirectoryName(Path.GetDirectoryName(Path.GetDirectoryName(Path.GetFullPath(tools).TrimEnd('/'))));
            var guess = root != null ? Path.Combine(root, "usr", "python", "bin", "python3") : null;
            py = guess != null && File.Exists(guess) ? guess : "python3";
        }
        return new ToolSet(tools, py);
    }

    /// <summary>What an unpacking that was cut off left behind: a tools.part of up to a gigabyte that is never used and stood in
    /// the way of the next try's room, and a tools.old whose removal failed (the tools that were there are the rollback point).</summary>
    public void Sweep()
    {
        try { if (Directory.Exists(Folder + ".part")) Directory.Delete(Folder + ".part", true); } catch (Exception) { }
        try { if (!Directory.Exists(Folder) && Directory.Exists(Folder + ".old")) Directory.Move(Folder + ".old", Folder); } catch (Exception) { }
        try { if (Directory.Exists(Folder) && Directory.Exists(Folder + ".old")) Directory.Delete(Folder + ".old", true); } catch (Exception) { }
    }

    /// <summary>Unpacks Triforce-Tools.zip as the tools folder (the old one replaced only once the new one is complete).</summary>
    public void Unpack(string zip, string tag, Action<double>? progress)
    {
        var target = Folder;
        ctx.Require(target);
        var part = target + ".part";
        if (Directory.Exists(part)) Directory.Delete(part, true);
        try
        {
            using (var z = ZipFile.OpenRead(zip))
            {
                // (room for all of it, asked before the first file: the zip says what each one unpacks to, and 4 KB are counted for
                // each on top, since a file takes whole clusters. The tools there now stay until the new ones are complete)
                var bytes = z.Entries.Sum(e => e.Length) + z.Entries.Count * 4096L;
                if (ctx.SpaceProblem("the Triforce tools", bytes, part) is { } none) throw new NoRoomException(none);
                Directory.CreateDirectory(part);
                ExtractChecked(z, part, progress);
            }
            File.WriteAllText(Path.Combine(part, "version.txt"), tag + "\n");
            // ((Triforce) a whole zip's base is the one its list names: the next update keeps it without reading it again)
            if (ReadBaseList(Path.Combine(part, BaseList)) is { } listed) File.WriteAllText(Path.Combine(part, BaseHave), listed.Id + "\n");
        }
        catch (Exception)
        {
            // (half a tools folder, a gigabyte of it, is of no use and stood in the way of the next try's room)
            try { if (Directory.Exists(part)) Directory.Delete(part, true); } catch (Exception) { }
            throw;
        }
        SwapIn(part, target, null);
        progress?.Invoke(1);
    }

    /// <summary>tools.part into tools' place: tools to tools.old, tools.part to tools; a failure puts the old folder back (a virus
    /// scanner holding the new one left no tools folder at all, and the next start found no tools).</summary>
    static void SwapIn(string part, string target, Action? putBack)
    {
        var old = target + ".old";
        if (Directory.Exists(old)) Directory.Delete(old, true);
        if (Directory.Exists(target)) Directory.Move(target, old);
        try { Directory.Move(part, target); }
        catch (Exception)
        {
            try
            {
                if (!Directory.Exists(target) && Directory.Exists(old)) Directory.Move(old, target);
                putBack?.Invoke();
            }
            catch (Exception) { }
            throw;
        }
        try { if (Directory.Exists(old)) Directory.Delete(old, true); } catch (Exception) { }
    }

    /// <summary>A tools zip's files into folder. The prefix tools/ is stripped when every entry has it (\ read as /). Every name is
    /// checked before the first file is written, as a mod archive's are (Mods.Unsafe): a device name, a colon, a name that ends in
    /// a dot or a space, a path out of the folder. Such a zip is refused whole.</summary>
    static void ExtractChecked(ZipArchive z, string folder, Action<double>? progress)
    {
        var prefix = z.Entries.All(e => e.FullName.Replace('\\', '/').StartsWith("tools/", StringComparison.OrdinalIgnoreCase)) ? "tools/" : "";
        foreach (var e in z.Entries)
        {
            var name = e.FullName.Replace('\\', '/').Substring(prefix.Length);
            if (name.Length > 0 && Mods.Unsafe(name, true) is { } why)
                throw new InvalidDataException("The tools' zip holds " + why + " (\"" + (name.Length > 120 ? name.Substring(0, 117) + "..." : name) + "\"): it was not unpacked.");
        }
        var full = Path.GetFullPath(folder) + Path.DirectorySeparatorChar;
        var cmp = OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
        for (var i = 0; i < z.Entries.Count; i++)
        {
            var e = z.Entries[i];
            var name = e.FullName.Replace('\\', '/').Substring(prefix.Length);
            if (name.Length == 0) continue;
            var dest = Path.GetFullPath(Path.Combine(folder, name));
            if (!dest.StartsWith(full, cmp)) continue;   // (no paths out of the folder)
            if (name.EndsWith('/')) { Directory.CreateDirectory(dest); continue; }
            Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
            e.ExtractToFile(dest, true);
            if ((i & 15) == 0) progress?.Invoke((double)i / z.Entries.Count);
        }
    }

    void Extract(string zip, string folder, Action<double>? progress)
    {
        using var z = ZipFile.OpenRead(zip);
        var bytes = z.Entries.Sum(e => e.Length) + z.Entries.Count * 4096L;
        if (ctx.SpaceProblem("the Triforce tools", bytes, folder) is { } none) throw new NoRoomException(none);
        ExtractChecked(z, folder, progress);
    }

    // ---- (Triforce, 11-18) the tools in two parts: the base kept from one release to the next ----
    // setup/pack_tools.ps1 lists the base (the compiler, Python, the libraries: the folders that do not change between releases,
    // 95 % of Triforce-Tools.zip) in tools/base.list, which the code part and the whole zip carry. An update of tools that hold
    // that base fetches the code part alone and moves the base folders over from the tools there now. The base is the list's
    // when every file it names is there with its size and SHA-256 (read once: base.have then remembers the list's id, and later
    // updates check the sizes alone). Anything else fetches the base part as well.
    public const string BaseList = "base.list", BaseHave = "base.have";

    public sealed class Base
    {
        public string Id = "";   // (the SHA-256 of the list's dir and file lines)
        public readonly List<string> Dirs = new();
        public readonly List<(string Path, long Size, string Sha)> Files = new();
    }

    /// <summary>A base list's text: "triforce-tools-base\t1", "id\t&lt;sha256&gt;", then "dir\t&lt;name&gt;" and
    /// "file\t&lt;size&gt;\t&lt;sha256&gt;\t&lt;path&gt;" lines (paths with /). Null when it is not one; an unknown line makes it none.</summary>
    public static Base? ParseBaseList(string text)
    {
        var lines = text.Replace("\r", "").Split('\n');
        if (lines.Length < 2 || lines[0].Trim() != "triforce-tools-base\t1") return null;
        var b = new Base();
        foreach (var line in lines.Skip(1))
        {
            var f = line.Split('\t');
            if (f.Length == 2 && f[0] == "id") b.Id = f[1].Trim().ToLowerInvariant();
            else if (f.Length == 2 && f[0] == "dir" && f[1].Length > 0 && Mods.Unsafe(f[1], true) == null && !f[1].Contains('/')) b.Dirs.Add(f[1]);
            else if (f.Length == 4 && f[0] == "file" && long.TryParse(f[1], out var size) && f[2].Length == 64 && Mods.Unsafe(f[3], true) == null)
                b.Files.Add((f[3], size, f[2].ToLowerInvariant()));
            else if (line.Length > 0) return null;   // (a line it does not know: another format, not guessed at)
        }
        return b.Id.Length == 64 && b.Dirs.Count > 0 && b.Files.Count > 0 ? b : null;
    }

    public static Base? ReadBaseList(string path)
    {
        try { return File.Exists(path) ? ParseBaseList(File.ReadAllText(path)) : null; } catch (Exception) { return null; }
    }

    /// <summary>The base list in a code (or whole) zip, null when it has none.</summary>
    public static Base? BaseListIn(string zip)
    {
        using var z = ZipFile.OpenRead(zip);
        var e = z.Entries.FirstOrDefault(x => x.FullName.Replace('\\', '/') is var n && (n == "tools/" + BaseList || n == BaseList));
        if (e == null) return null;
        using var r = new StreamReader(e.Open());
        return ParseBaseList(r.ReadToEnd());
    }

    /// <summary>Whether the tools in folder hold base b whole (progress: 0..1 while it reads the files). Remembers a match in base.have.</summary>
    public static bool HasBase(Base b, string folder, Action<double>? progress)
    {
        foreach (var d in b.Dirs)
            if (!Directory.Exists(Path.Combine(folder, d))) return false;
        foreach (var f in b.Files)   // (every file there, at its size: quick, and enough once base.have says the rest was read)
        {
            var fi = new FileInfo(Path.Combine(folder, f.Path));
            if (!fi.Exists || fi.Length != f.Size) return false;
        }
        string? had = null;
        try { had = File.Exists(Path.Combine(folder, BaseHave)) ? File.ReadAllText(Path.Combine(folder, BaseHave)).Trim() : null; } catch (Exception) { }
        if (had == b.Id) return true;
        long total = Math.Max(1, b.Files.Sum(f => f.Size)), done = 0;
        foreach (var f in b.Files)
        {
            string got;
            using (var s = File.OpenRead(Path.Combine(folder, f.Path))) got = Convert.ToHexStringLower(SHA256.HashData(s));
            if (got != f.Sha) return false;
            done += f.Size;
            progress?.Invoke((double)done / total);
        }
        try { File.WriteAllText(Path.Combine(folder, BaseHave), b.Id + "\n"); } catch (Exception) { }
        return true;
    }

    /// <summary>The tools folder made again from a code zip and either a base zip (baseZip) or the base folders of the tools there
    /// now (baseZip null: HasBase said they are b's). The old folder is replaced only once the new one is complete; what fails on
    /// the way puts the base folders back where they were.</summary>
    public void UnpackParts(string codeZip, string? baseZip, Base b, string tag, Action<double>? progress)
    {
        var target = Folder;
        ctx.Require(target);
        var part = target + ".part";
        if (Directory.Exists(part)) Directory.Delete(part, true);
        var moved = new List<string>();
        void PutBack()
        {
            foreach (var d in moved)
                try { if (Directory.Exists(target) && !Directory.Exists(Path.Combine(target, d))) Directory.Move(Path.Combine(part, d), Path.Combine(target, d)); }
                catch (Exception) { }
        }
        try
        {
            Directory.CreateDirectory(part);
            Extract(codeZip, part, f => progress?.Invoke(baseZip == null ? f * 0.9 : f * 0.1));
            if (baseZip != null) Extract(baseZip, part, f => progress?.Invoke(0.1 + f * 0.8));
            else
                foreach (var d in b.Dirs)
                {
                    var into = Path.Combine(part, d);
                    if (Directory.Exists(into)) Directory.Delete(into, true);   // (a code zip has none of them; never mixed)
                    Directory.Move(Path.Combine(target, d), into);
                    moved.Add(d);
                }
            foreach (var f in b.Files)   // (the base whole in the new folder, whichever way it came)
            {
                var fi = new FileInfo(Path.Combine(part, f.Path));
                if (!fi.Exists || fi.Length != f.Size) throw new InvalidDataException("The tools' base lacks " + f.Path + " (or it has another size): they were not changed.");
            }
            File.WriteAllText(Path.Combine(part, "version.txt"), tag + "\n");
            File.WriteAllText(Path.Combine(part, BaseHave), b.Id + "\n");
        }
        catch (Exception)
        {
            PutBack();
            try { if (Directory.Exists(part)) Directory.Delete(part, true); } catch (Exception) { }
            throw;
        }
        SwapIn(part, target, PutBack);
        progress?.Invoke(1);
    }
}

/// <summary>Merge's step 2 (old Shell.cs Merge and ToolsInParts): the release's tools into &lt;install&gt;\tools, downloaded through
/// Downloader into &lt;install&gt;\downloads\Triforce-Tools-&lt;tag&gt;.zip with the sizes the release lists. Windows only: on Linux the
/// tools come with the AppImage.</summary>
public sealed class ToolsInstaller
{
    readonly TriforceContext ctx;
    readonly Tools tools;
    readonly Downloader downloader;
    public ToolsInstaller(TriforceContext ctx, Downloader? downloader = null)
    {
        this.ctx = ctx;
        tools = new Tools(ctx);
        this.downloader = downloader ?? new Downloader(ctx);
    }

    // (what the tools take unpacked, until their zip is here to say it exactly: v0.4.0's Triforce-Tools.zip is 284 MB and unpacks
    // to 1.1 GB in 13,230 files, 3.9 times its size)
    public const int UnpackedTimes = 5;

    public event Action<ToolsProgress>? Progress;
    void Say(ToolsStep step, double f, long bytes, long total, string text) => Progress?.Invoke(new ToolsProgress(step, f, bytes, total, text));

    /// <summary>Are the release's tools another than the ones here?</summary>
    public bool Needed(Release r) => tools.InstalledVersion != r.Tag;

    static string SafeTag(string tag) => string.Concat(tag.Where(c => char.IsLetterOrDigit(c) || c == '.' || c == '-'));
    static long Length(string file) { try { return File.Exists(file) ? new FileInfo(file).Length : 0; } catch (Exception) { return 0; } }

    /// <summary>The tools of release r, when the ones here are not its own. Parts when the tools are here and the release has its
    /// code and base apart (the code alone, the base only when the one here is not the release's); otherwise, or when the code
    /// part carries no base list, the whole zip. A zip that is there whole (by its size) is not fetched again.</summary>
    public async Task UpdateAsync(HttpClient http, Release r, CancellationToken token)
    {
        if (!Needed(r)) return;
        if (r.ToolsUrl == null) throw new InvalidOperationException("The release " + r.Tag + " has no " + BuildInfo.ToolsAsset + ".");
        var have = tools.InstalledVersion;
        var cache = ctx.DownloadsFolder;
        ctx.Require(cache);
        Directory.CreateDirectory(cache);
        var zip = Path.Combine(cache, "Triforce-Tools-" + SafeTag(r.Tag) + ".zip");
        await Task.Run(tools.Sweep, token).ConfigureAwait(false);
        var parts = have != null && r.ToolsCodeUrl != null && r.ToolsBaseUrl != null && await InPartsAsync(http, r, have, cache, token).ConfigureAwait(false);
        if (!parts)
        {
            var whole = r.ToolsSize > 0 && Length(zip) == r.ToolsSize;
            var unpacked = r.ToolsSize * UnpackedTimes;
            if (r.ToolsSize > 0 && ctx.SpaceProblem("the Triforce tools (a download of " + (r.ToolsSize >> 20) + " MB that unpacks to about " + (unpacked >> 20) + " MB)",
                    (whole ? 0 : Math.Max(0, r.ToolsSize - Length(zip + ".part"))) + unpacked, cache) is { } none)
                throw new NoRoomException(none);
            var text = have == null ? "Downloading Triforce's tools" : "Downloading the tools of " + r.Tag;
            Say(ToolsStep.Download, 0, 0, r.ToolsSize, text);
            if (!whole)
                await downloader.ToFileAsync(http, r.ToolsUrl, zip, r.ToolsSize, p => Say(ToolsStep.Download, p.Fraction, p.Bytes, p.Total, text), token).ConfigureAwait(false);
            else ctx.Log.Write("tools " + r.Tag + ": the zip is here already, whole (" + r.ToolsSize + " bytes)");
            Say(ToolsStep.Unpack, 0, 0, 0, "Unpacking the tools");
            try { await Task.Run(() => tools.Unpack(zip, r.Tag, f => Say(ToolsStep.Unpack, f, 0, 0, "Unpacking the tools")), CancellationToken.None).ConfigureAwait(false); }
            catch (Exception e) when (e is not NoRoomException && !ctx.Platform.IsDiskFull(e))
            {
                // (a zip that does not unpack is fetched again at the next try, not tried again; one there was no room for is kept)
                try { File.Delete(zip); } catch (Exception) { }
                throw;
            }
            Leavings(cache);
            ctx.Log.Write("tools " + r.Tag + " unpacked into " + tools.Folder);
        }
        Say(ToolsStep.Done, 1, 0, 0, "The tools are ready");
    }

    /// <summary>The downloads of this and other releases: parts nothing will go on from.</summary>
    static void Leavings(string cache)
    {
        string[] leavings = { };
        try { leavings = Directory.GetFiles(cache, "Triforce-Tools-*"); } catch (Exception) { }
        foreach (var f in leavings)
            try { File.Delete(f); } catch (Exception) { }
    }

    async Task<bool> InPartsAsync(HttpClient http, Release r, string have, string cache, CancellationToken token)
    {
        var tag = SafeTag(r.Tag);
        var code = Path.Combine(cache, "Triforce-Tools-code-" + tag + ".zip");
        var baseZip = Path.Combine(cache, "Triforce-Tools-base-" + tag + ".zip");
        var text = "Downloading the new code of " + r.Tag + "; the compiler and Python you have are kept";
        Say(ToolsStep.Download, 0, 0, r.ToolsCodeSize, text);
        if (!(r.ToolsCodeSize > 0 && Length(code) == r.ToolsCodeSize))
            await downloader.ToFileAsync(http, r.ToolsCodeUrl!, code, r.ToolsCodeSize, p => Say(ToolsStep.Download, p.Fraction, p.Bytes, p.Total, text), token).ConfigureAwait(false);
        var b = await Task.Run(() => { try { return Tools.BaseListIn(code); } catch (Exception) { return null; } }, token).ConfigureAwait(false);
        if (b == null)
        {
            ctx.Log.Write("tools " + r.Tag + ": the code part names no base: the whole zip instead");
            try { File.Delete(code); } catch (Exception) { }
            return false;
        }
        Say(ToolsStep.CheckBase, 0, 0, 0, "Checking the tools you have");
        var keep = await Task.Run(() => { try { return Tools.HasBase(b, tools.Folder, f => Say(ToolsStep.CheckBase, f, 0, 0, "Checking the tools you have")); } catch (Exception) { return false; } }, token).ConfigureAwait(false);
        ctx.Log.Write("tools " + r.Tag + ": the base (" + b.Files.Count + " files) " + (keep ? "is the one here: kept" : "is not the one here: fetched"));
        if (!keep)
        {
            var unpacked = r.ToolsBaseSize * UnpackedTimes;
            if (r.ToolsBaseSize > 0 && ctx.SpaceProblem("the Triforce tools (a download of " + (r.ToolsBaseSize >> 20) + " MB that unpacks to about " + (unpacked >> 20) + " MB)",
                    Math.Max(0, r.ToolsBaseSize - Length(baseZip + ".part")) + unpacked, cache) is { } none)
                throw new NoRoomException(none);
            var bt = "Downloading the compiler and Python (" + (r.ToolsBaseSize >> 20) + " MB)";
            Say(ToolsStep.DownloadBase, 0, 0, r.ToolsBaseSize, bt);
            if (!(r.ToolsBaseSize > 0 && Length(baseZip) == r.ToolsBaseSize))
                await downloader.ToFileAsync(http, r.ToolsBaseUrl!, baseZip, r.ToolsBaseSize, p => Say(ToolsStep.DownloadBase, p.Fraction, p.Bytes, p.Total, bt), token).ConfigureAwait(false);
        }
        Say(ToolsStep.Unpack, 0, 0, 0, "Unpacking the tools");
        try { await Task.Run(() => tools.UnpackParts(code, keep ? null : baseZip, b, r.Tag, f => Say(ToolsStep.Unpack, f, 0, 0, "Unpacking the tools")), CancellationToken.None).ConfigureAwait(false); }
        catch (Exception e) when (e is not NoRoomException && !ctx.Platform.IsDiskFull(e))
        {
            // (parts that do not unpack are fetched again at the next try, not tried again)
            try { File.Delete(code); } catch (Exception) { }
            try { File.Delete(baseZip); } catch (Exception) { }
            throw;
        }
        Leavings(cache);
        ctx.Log.Write("tools " + r.Tag + " unpacked into " + tools.Folder + " from its parts (" + (keep ? "the base kept" : "the base fetched") + ")");
        return true;
    }
}
