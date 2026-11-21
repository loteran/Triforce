// The player's own game files, as the build takes them (old Shell.cs:885-958, research/02 section 4, research/12 section 3):
// a .nup file, or romfs + exefs folders. One path from a drop or a picker is turned into a source; quick checks run without
// the tools (the file is there, a .nup's header reads, the folders are there, exefs\main is the supported build, no NX2navi
// packed romfs); the full check is make_game.py --check (the tools read the .nup's exefs).
using System.Diagnostics;
using System.Text;

namespace Triforce.Logic;

public enum SourceKind { Nup, Folders }

/// <summary>The game files a build reads. For Folders, Romfs and Exefs (either may be missing: Missing names it).</summary>
public sealed record GameSource(SourceKind Kind, string? Nup, string? Romfs, string? Exefs)
{
    /// <summary>The source is there whole (old SourceGiven: a .nup that exists; a romfs folder and exefs\main).</summary>
    public bool Complete => Kind == SourceKind.Nup ? File.Exists(Nup) : Directory.Exists(Romfs) && Exefs != null && File.Exists(Path.Combine(Exefs, "main"));
    /// <summary>What is still to be chosen: "romfs", "exefs", or null.</summary>
    public string? Missing => Kind == SourceKind.Nup ? null : !Directory.Exists(Romfs) ? "romfs" : Exefs == null || !Directory.Exists(Exefs) ? "exefs" : null;
    /// <summary>The name the file card shows.</summary>
    public string DisplayName => Kind == SourceKind.Nup ? Path.GetFileName(Nup!) : Path.GetFileName(Path.GetDirectoryName((Romfs ?? Exefs ?? "").TrimEnd('\\', '/')) ?? "") is { Length: > 0 } n ? n : Romfs ?? Exefs ?? "";
    /// <summary>make_game.py's arguments for it (--nup f, or --romfs d --exefs d).</summary>
    public IReadOnlyList<string> Args => Kind == SourceKind.Nup ? new[] { "--nup", Nup! } : new[] { "--romfs", Romfs ?? "", "--exefs", Exefs ?? "" };
}

/// <summary>One title in a .nup package (its header's table): the base game or an update.</summary>
public sealed record NupTitle(string Id, uint Version);

/// <summary>A quick answer, without the tools.</summary>
public sealed record QuickCheck(bool Ok, string Text, IReadOnlyList<NupTitle>? Titles = null, string? BuildId = null)
{
    /// <summary>The answer is final (a problem, or a folder whose build id was read): the tools' check adds nothing for folders.</summary>
    public bool Final => !Ok || BuildId != null;
}

/// <summary>make_game.py --check's answer.</summary>
public sealed record CheckResult(bool Ok, string Text, string? BuildId, bool ToolsMissing = false);

/// <summary>Room for a build on a disk (DESIGN 6: "Takes 60 GB while building, 24 GB after. 212 GB free on D:").</summary>
public sealed record SpaceInfo(string Disk, long? Free, long NeedWhileBuilding, long NeedAfter)
{
    /// <summary>Not enough: the line turns ruby and the Build button is off (the old Setup only asked; DESIGN 6 turns it off).</summary>
    public bool TooLittle => Free is { } f && f < NeedWhileBuilding;
    public string Text => "Takes " + (NeedWhileBuilding >> 30) + " GB while building, about " + (NeedAfter >> 30) + " GB after." +
                          (Free is { } f ? " " + (f >> 30) + " GB free on " + Disk.TrimEnd('\\', '/') + "." : "");
}

public sealed class GameIntake
{
    readonly TriforceContext ctx;
    public GameIntake(TriforceContext ctx) { this.ctx = ctx; }

    /// <summary>The build id the patches, the hot list and the profile were made from: the supported game version is identified
    /// only by this (make_game.py BUILD_ID; there is no literal "1.0.0" anywhere).</summary>
    public const string SupportedBuildId = "e0f3ccfa";
    /// <summary>What a build takes while it runs (old Shell.cs NeededBytes).</summary>
    public const long NeededWhileBuilding = 60L << 30;
    /// <summary>About what a built game folder keeps (DESIGN 6). An estimate, said with "about".</summary>
    public const long NeededAfter = 24L << 30;

    /// <summary>make_game.py PACKED_ROMFS: NX2navi's romfs folder holds one packed romfs.bin, which the builder cannot read.</summary>
    public const string PackedRomfs = "This romfs folder is NX2navi's: it holds the game's data packed in one file (romfs.bin), which Triforce cannot " +
                                      "read, so the game would crash at start. Choose your game file (.nup) instead, or romfs and exefs folders with " +
                                      "the game's files unpacked in them.";

    /// <summary>One dropped or picked path as a source (old Shell.cs Dropped): a *.nup; a folder named romfs or exefs (the other one
    /// beside it is taken along when it is there); a folder holding romfs and exefs; a built game folder keeps its exefs as
    /// .triforce_exefs beside romfs. merge: a source the player gave before, whose other half this path may be. Null when the
    /// path is none of these.</summary>
    public static GameSource? Normalize(string path, GameSource? merge = null)
    {
        if (string.IsNullOrWhiteSpace(path)) return null;
        path = path.Trim().Trim('"');
        try { path = Path.GetFullPath(path).TrimEnd('\\', '/'); } catch (Exception) { return null; }
        var name = Path.GetFileName(path).ToLowerInvariant();
        if (name.EndsWith(".nup", StringComparison.Ordinal) && !Directory.Exists(path)) return new GameSource(SourceKind.Nup, path, null, null);
        if (!Directory.Exists(path)) return null;
        var parent = Path.GetDirectoryName(path);
        var keep = merge is { Kind: SourceKind.Folders } ? merge : null;
        if (name == "romfs")
            return new GameSource(SourceKind.Folders, null, path, ExefsBeside(parent) ?? keep?.Exefs);
        if (name == "exefs" || name == ".triforce_exefs" || name == ".hookshot_exefs")
            return new GameSource(SourceKind.Folders, null, parent != null && Directory.Exists(Path.Combine(parent, "romfs")) ? Path.Combine(parent, "romfs") : keep?.Romfs, path);
        if (Directory.Exists(Path.Combine(path, "romfs")) && ExefsBeside(path) is { } exefs)
            return new GameSource(SourceKind.Folders, null, Path.Combine(path, "romfs"), exefs);
        // (a folder with one .nup in it: that file)
        try
        {
            var nups = Directory.GetFiles(path, "*.nup");
            if (nups.Length == 1) return new GameSource(SourceKind.Nup, nups[0], null, null);
        }
        catch (Exception) { }
        return null;
    }

    static string? ExefsBeside(string? folder)
    {
        if (folder == null) return null;
        foreach (var n in new[] { "exefs", ".triforce_exefs", ".hookshot_exefs" })
            if (Directory.Exists(Path.Combine(folder, n))) return Path.Combine(folder, n);
        return null;
    }

    /// <summary>The checks that need no tools. A .nup: there, and its header reads ("NUP\x01", a table of titles). Folders: there,
    /// exefs\main an NSO whose build id is read (0x40..0x60 of its header) and compared, and no packed romfs.bin.</summary>
    public static QuickCheck Quick(GameSource s)
    {
        if (s.Kind == SourceKind.Nup)
        {
            if (!File.Exists(s.Nup)) return new QuickCheck(false, "The file is not there: " + s.Nup);
            try
            {
                var titles = ReadNupTitles(s.Nup!);
                if (titles.Count == 0) return new QuickCheck(false, "This .nup file holds no game.");
                if (!titles.Any(t => t.Id.Equals("0400486024dec000", StringComparison.OrdinalIgnoreCase)))
                    return new QuickCheck(false, "This .nup file holds another game (" + string.Join(", ", titles.Select(t => t.Id.ToUpperInvariant())) + "), not Ocarina of Time (0400486024DEC000).", titles);
                return new QuickCheck(true, "The game file reads.", titles);
            }
            catch (Exception e) { return new QuickCheck(false, "This is not a .nup game file Triforce can read (" + e.Message + ")."); }
        }
        if (s.Missing == "romfs") return new QuickCheck(false, "Choose the romfs folder too.");
        if (s.Romfs != null && File.Exists(Path.Combine(s.Romfs, "romfs.bin"))) return new QuickCheck(false, PackedRomfs);
        if (s.Missing == "exefs") return new QuickCheck(false, "Choose the exefs folder too.");
        var main = Path.Combine(s.Exefs!, "main");
        if (!File.Exists(main)) return new QuickCheck(false, "The exefs folder has no main file: it is not the game's exefs.");
        var bid = BuildIdOf(main);
        if (bid == null) return new QuickCheck(false, main + " is not a game executable");
        return bid.StartsWith(SupportedBuildId, StringComparison.Ordinal)
            ? new QuickCheck(true, "This is the version Triforce supports (build " + bid.Substring(0, 8) + ").", null, bid)
            : new QuickCheck(false, "This is build " + bid.Substring(0, 8) + ". Triforce supports build " + SupportedBuildId + ".", null, bid);
    }

    /// <summary>The module id of an NSO, plain or compressed (the header is the same): bytes 0x40..0x60 as hex; null when it is no
    /// NSO (make_game.py build_id).</summary>
    public static string? BuildIdOf(string main)
    {
        try
        {
            using var f = new FileStream(main, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            var head = new byte[0x60];
            if (f.ReadAtLeast(head, head.Length, false) < head.Length) return null;
            if (head[0] != 'N' || head[1] != 'S' || head[2] != 'O' || head[3] != '0') return null;
            return Convert.ToHexStringLower(head, 0x40, 0x20);
        }
        catch (Exception) { return null; }
    }

    /// <summary>The titles in a .nup's header, newest version first. Opened read-only, sharing reads: a .nup is never written.
    /// Layout (nx2/nup.py): a 0x80-byte header ("NUP\x01", u64 file size at +0x40), then at 0x80 the TOC ("TOC\x01", u32 titles at
    /// +4), the titles 0x40 each from 0xb0: u64 title id, u32 root, u32 exefs, u32 romfs, u32 version. A package shorter than its
    /// header says is a download or copy that did not finish (nup.py says so too, but 30 minutes into a build it once did).</summary>
    public static IReadOnlyList<NupTitle> ReadNupTitles(string nup)
    {
        using var f = new FileStream(nup, FileMode.Open, FileAccess.Read, FileShare.Read);
        var head = new byte[0x80];
        if (f.ReadAtLeast(head, head.Length, false) < head.Length) throw new InvalidDataException("the file is too short");
        if (head[0] != 'N' || head[1] != 'U' || head[2] != 'P' || head[3] != 1) throw new InvalidDataException("not a NUP package");
        var size = BitConverter.ToInt64(head, 0x40);
        if (size > 0 && f.Length < size)
            throw new InvalidDataException("the .nup file is incomplete: it is " + (f.Length >> 20) + " MB, its header says " + (size >> 20) +
                                           " MB. Download or copy it again (wait for the download to finish) and check that the drive had room");
        var t = new byte[0x30];
        if (f.ReadAtLeast(t, t.Length, false) < t.Length || t[0] != 'T' || t[1] != 'O' || t[2] != 'C' || t[3] != 1) throw new InvalidDataException("no table of contents");
        var count = BitConverter.ToUInt32(t, 4);
        if (count == 0 || count > 64) throw new InvalidDataException("an odd number of titles (" + count + ")");
        var titles = new List<NupTitle>();
        var rec = new byte[0x40];
        for (var i = 0; i < count; i++)
        {
            if (f.ReadAtLeast(rec, rec.Length, false) < rec.Length) throw new InvalidDataException("the title table is cut off");
            titles.Add(new NupTitle(BitConverter.ToUInt64(rec, 0).ToString("x16"), BitConverter.ToUInt32(rec, 0x14)));
        }
        return titles.OrderByDescending(x => x.Version).ToList();
    }


    /// <summary>The full check (old Runner.Check; make_game.py check): --check --nup f, or --check --exefs d --romfs d (the romfs too:
    /// NX2navi's packed romfs.bin is refused now, not after a 45-minute build). Without tools: ToolsMissing.</summary>
    public async Task<CheckResult> CheckAsync(GameSource s, CancellationToken token = default)
    {
        var tools = new Tools(ctx).Find();
        if (tools == null) return new CheckResult(false, "The Triforce tools are not here yet: they come with the first update check.", null, ToolsMissing: true);
        var args = new List<string> { "--check" };
        args.AddRange(s.Kind == SourceKind.Nup ? new[] { "--nup", s.Nup! } : new[] { "--exefs", s.Exefs ?? "", "--romfs", s.Romfs ?? "" });
        var result = new CheckResult(false, "The game files could not be read.", null);
        Process p;
        try { p = Process.Start(BuildRunner.StartInfo(ctx, tools, args, null, dlss: false))!; }
        catch (Exception)
        {
            if (ctx.IsWindows && Directory.Exists(Path.Combine(tools.Root, "python")) && !File.Exists(Path.Combine(tools.Root, "python", "python.exe")))
                return new CheckResult(false, "Triforce's own Python (tools\\python\\python.exe) is missing. Your antivirus probably removed it: restore it from " +
                                              "quarantine, or start Triforce again to download the tools once more.", null);
            return new CheckResult(false, "Python was not found. Start Triforce again to download the Triforce tools.", null);
        }
        using (p)
        using (token.Register(() => { try { ctx.Platform.KillTree(p); } catch (Exception) { } }))
        {
            p.ErrorDataReceived += (_, _) => { };
            p.BeginErrorReadLine();
            for (string? line; (line = await p.StandardOutput.ReadLineAsync(token).ConfigureAwait(false)) != null;)
                if (BuildProtocol.Parse(line) is { } e)
                    result = e switch
                    {
                        BuildEvent.Check c => new CheckResult(c.Ok, c.Text, ParseBuildId(c.Text)),
                        BuildEvent.Error x => new CheckResult(false, x.Text, null),
                        _ => result,
                    };
            await p.WaitForExitAsync(token).ConfigureAwait(false);
        }
        ctx.Log.Write("check " + string.Join(" ", args) + ": " + (result.Ok ? "OK " : "BAD ") + result.Text);
        return result;
    }

    static string? ParseBuildId(string text)
    {
        var m = System.Text.RegularExpressions.Regex.Match(text, @"build ([0-9a-f]{8})");
        return m.Success ? m.Groups[1].Value : null;
    }

    /// <summary>The words the add screen shows for a bad check (old: the localized VersionBad when the text starts "This is build").</summary>
    public static bool IsWrongVersion(string text) => text.StartsWith("This is build", StringComparison.Ordinal);

    /// <summary>Room where the game is built.</summary>
    public SpaceInfo Space(string outFolder) => new(ctx.Platform.Disk(outFolder), ctx.FreeBytes(outFolder), NeededWhileBuilding, NeededAfter);
}
