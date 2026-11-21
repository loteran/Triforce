// Linux builder updates (builder/linux/linux_update.py; research/12 section 5).
//
// !!! BROKEN ON CURRENT RELEASES (research/12 5.4, verified from the release files 11-19): the parts self-update cannot work
// today. The Forgejo releases (v0.5.0, v0.5.1) carry only Triforce-Tools.zip, the Linux AppImage and TriforceSetup.exe: there
// is NO triforce-linux.txt, so FetchManifestAsync gets a 404 and every "Update builder" ends in an error. And the part URLs that
// scripts/linux/appimage.sh writes point at https://github.com/NotRexed/Hookshot/releases/download/linux-parts (no
// GITHUB_REPOSITORY on the hand-built releases), which publish_parts.sh uploads to GitHub only. Until the release process
// publishes parts and the list to Forgejo, a Linux player updates by downloading the new AppImage (Release.LinuxAppImageUrl),
// which is what the launcher should offer. The parts INSTALLER (linux_update.install: clone unchanged parts with hard links,
// download the rest, sha256 + size check, unzip with path and symlink guards, rebase the sysroot's links, versions/<tag>.part
// -> versions/<tag>, current) is NOT ported: TODO once the parts are published again.
//
// What is ported, as pure logic (with tests/test_linux_update.py's cases in LogicTests): the list's format, release_key,
// update_target, choose_mode, tools_tag, game_version, and where the installed builder is (current, versions/<tag>/AppRun),
// which shipped games (host/hos_update.c) and folder scripts (hookshot-folder) read.
using System.Text.RegularExpressions;

namespace Triforce.Logic;

public sealed record LinuxPart(string Sha, long Size, string Dest, string Url, IReadOnlyList<string>? Files);

public sealed record LinuxManifest(string Tag, IReadOnlyList<LinuxPart> Parts);

/// <summary>A version to compare (linux_update.release_key): the numbers, then a prerelease before its release, then commits after
/// a tag (or changes to it) after that tag and before the next.</summary>
public sealed record ReleaseKey(IReadOnlyList<int> Numbers, bool Released, bool After) : IComparable<ReleaseKey>
{
    public int CompareTo(ReleaseKey? o)
    {
        if (o == null) return 1;
        for (var i = 0; i < Math.Min(Numbers.Count, o.Numbers.Count); i++)
            if (Numbers[i] != o.Numbers[i]) return Numbers[i].CompareTo(o.Numbers[i]);
        if (Numbers.Count != o.Numbers.Count) return Numbers.Count.CompareTo(o.Numbers.Count);
        if (Released != o.Released) return Released ? 1 : -1;
        return After == o.After ? 0 : After ? 1 : -1;
    }
}

public static class LinuxUpdate
{
    public const string LatestList = BuildInfo.Downloads + "/releases/latest/download/" + BuildInfo.LinuxListAsset;
    static readonly Regex KeyPattern = new(@"^(?=v|\d+\.)v?(\d+(?:\.\d+)*)(?:-(?!\d+-g|dirty$)([0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*))?(-\d+-g[0-9a-f]+)?(-dirty)?$");

    /// <summary>A release tag (v2.0.5, v2.1.0-rc.1) or git describe's v2.0.5-3-g1a2b3c4[-dirty]; null for a build that names no
    /// release (a bare commit, dev, nothing).</summary>
    public static ReleaseKey? Key(string? tag)
    {
        var m = KeyPattern.Match((tag ?? "").Trim());
        if (!m.Success) return null;
        return new ReleaseKey(m.Groups[1].Value.Split('.').Select(int.Parse).ToList(), !m.Groups[2].Success, m.Groups[3].Success || m.Groups[4].Success);
    }

    /// <summary>The newest version this builder knows of: the latest release when it is newer than these tools, else the tools'
    /// own (issue 902: a test build newer than the release is not taken back to it).</summary>
    public static string? UpdateTarget(string? latest, string? tools)
    {
        if (string.IsNullOrEmpty(latest) || string.IsNullOrEmpty(tools)) return string.IsNullOrEmpty(latest) ? tools : latest;
        var have = Key(tools);
        var outKey = Key(latest);
        return have != null && outKey != null && outKey.CompareTo(have) > 0 ? latest : tools;
    }

    /// <summary>Is the release a newer builder than these tools?</summary>
    public static bool BuilderNewer(string? latest, string? tools) => !string.IsNullOrEmpty(latest) && !string.IsNullOrEmpty(tools) && UpdateTarget(latest, tools) != tools;

    /// <summary>What the builder's button does (choose_mode): "update-game" (an older game that still has its files),
    /// "up-to-date" (play), "update-builder", or "build".</summary>
    public static string ChooseMode(string? game, string? tools, string? latest, bool sourcePicked, bool kept)
    {
        var target = UpdateTarget(latest, tools);
        if (kept && !string.IsNullOrEmpty(game) && !string.IsNullOrEmpty(target) && game != target && !sourcePicked) return "update-game";
        if (!string.IsNullOrEmpty(game) && !sourcePicked) return "up-to-date";
        if (!string.IsNullOrEmpty(latest) && !string.IsNullOrEmpty(tools) && target != tools) return "update-builder";
        return "build";
    }

    /// <summary>The setup list (parse_list). local: a list from this PC, whose parts may be file:// links; a downloaded one's are
    /// https. Throws FormatException for anything else.</summary>
    public static LinuxManifest ParseList(string text, bool local = true)
    {
        var tag = "";
        var parts = new List<(string Sha, long Size, string Dest, string Url, List<string>? Files)>();
        int cur = -1;
        foreach (var raw in text.Replace("\r", "").Split('\n'))
        {
            var line = raw.Trim();
            if (line.Length == 0 || line.StartsWith('#')) continue;
            if (line.StartsWith("tag ", StringComparison.Ordinal)) tag = line.Substring(4).Trim();
            else if (line.StartsWith("part ", StringComparison.Ordinal))
            {
                var bits = line.Split(' ');
                if (bits.Length != 5 || !long.TryParse(bits[2], out var size)) throw new FormatException("a part line needs sha, size, directory and url");
                parts.Add((bits[1], size, bits[3], bits[4], new List<string>()));
                cur = parts.Count - 1;
            }
            else if (line == "tree")
            {
                if (cur < 0) throw new FormatException("tree with no part");
                parts[cur] = parts[cur] with { Files = null };
            }
            else if (line.StartsWith("file ", StringComparison.Ordinal))
            {
                if (cur < 0 || parts[cur].Files == null) throw new FormatException("file with no part");
                parts[cur].Files!.Add(line.Substring(5).Trim());
            }
            else throw new FormatException("unknown list line");
        }
        if (tag.Length == 0 || parts.Count == 0) throw new FormatException("the setup list has no release");
        // (the tag names a folder under versions/ that an update removes and remakes, and a part's names are paths in it)
        if (!Regex.IsMatch(tag, @"^[0-9A-Za-z._+-]{1,128}$") || tag == "." || tag == "..") throw new FormatException("the setup list's tag is not a version name");
        foreach (var p in parts)
        {
            if (!Regex.IsMatch(p.Sha, "^[0-9a-f]{64}$") || !(p.Url.StartsWith("https://", StringComparison.Ordinal) || local && p.Url.StartsWith("file://", StringComparison.Ordinal)))
                throw new FormatException("a part line has a bad checksum or link");
            if (p.Dest != "." && SafeName(p.Dest) is { } why) throw new FormatException(why);
            foreach (var n in p.Files ?? new List<string>())
                if (SafeName(n) is { } why2) throw new FormatException(why2);
        }
        return new LinuxManifest(tag, parts.Select(p => new LinuxPart(p.Sha, p.Size, p.Dest, p.Url, p.Files)).ToList());
    }

    /// <summary>A relative path that stays inside the folder it names a place in (linux_update._safe_name), null when it does.</summary>
    static string? SafeName(string name)
    {
        if (name.Length == 0 || name.StartsWith('/') || name.Split('/').Contains("..")) return "the zip has a path outside its folder";
        return null;
    }

    /// <summary>The release these tools are, from VERSION or version.txt (tools_tag).</summary>
    public static string ToolsTag(string? root)
    {
        if (string.IsNullOrEmpty(root)) return "";
        foreach (var rel in new[] { "usr/share/hookshot/VERSION", "usr/share/hookshot/version.txt", "VERSION", "version.txt" })
        {
            try
            {
                var path = Path.Combine(root, rel);
                if (!File.Exists(path)) continue;
                var line = File.ReadLines(path).FirstOrDefault()?.Trim();
                if (!string.IsNullOrEmpty(line)) return line;
            }
            catch (Exception) { }
        }
        return "";
    }

    /// <summary>The builder an update installed: data_root/versions/&lt;current&gt;/AppRun, null when none (hookshot-folder installed()).</summary>
    public static string? InstalledAppRun(string dataRoot)
    {
        try
        {
            var current = Path.Combine(dataRoot, "current");
            if (!File.Exists(current)) return null;
            var tag = File.ReadAllText(current).Replace(" ", "").Replace("\r", "").Replace("\n", "");
            var run = Path.Combine(dataRoot, "versions", tag, "AppRun");
            return tag.Length > 0 && File.Exists(run) ? run : null;
        }
        catch (Exception) { return null; }
    }

    /// <summary>The newest release's own list, by its tag (so a pre-release is found), else releases/latest
    /// (fetch_manifest). HOOKSHOT_LINUX_LIST=&lt;file&gt; overrides. SEE THE NOTE AT THE TOP: no release has a list today.</summary>
    public static async Task<LinuxManifest> FetchManifestAsync(TriforceContext ctx, HttpClient http, Release? latest, CancellationToken token)
    {
        if (ctx.Env("HOOKSHOT_LINUX_LIST") is { Length: > 0 } local) return ParseList(await File.ReadAllTextAsync(local, token).ConfigureAwait(false));
        var tag = latest?.Tag ?? "";
        if (latest?.LinuxListUrl is { } listed && listed.StartsWith("https://", StringComparison.Ordinal))
            return ParseList(await http.GetStringAsync(listed, token).ConfigureAwait(false), local: false);
        if (tag.Length > 0 && tag.All(c => char.IsLetterOrDigit(c) || "._-".Contains(c)))
        {
            try
            {
                return ParseList(await http.GetStringAsync(BuildInfo.Downloads + "/releases/download/" + tag + "/" + BuildInfo.LinuxListAsset, token).ConfigureAwait(false), local: false);
            }
            catch (HttpRequestException) { }
        }
        return ParseList(await http.GetStringAsync(LatestList, token).ConfigureAwait(false), local: false);
    }
}
