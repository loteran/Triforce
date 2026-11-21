// What the newest release offers (old Setup.cs Release): Forgejo's release list at triforceproject.com, newest first,
// pre-releases included, with GitHub's field names. TRIFORCE_RELEASE_API replaces the address (testing against a local copy).
using System.Net;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Triforce.Logic;

public sealed record ReleaseAsset(string Name, string? Url, long Size);

/// <summary>One "New in &lt;version&gt;" item on Home: a title and one line (DESIGN 6 Home, 7).</summary>
public sealed record NewInItem(string Title, string Text);

public sealed class Release
{
    public string Tag { get; set; } = "";
    public string Name { get; set; } = "";
    /// <summary>The release notes (Markdown).</summary>
    public string Body { get; set; } = "";
    public bool Prerelease { get; set; }
    public DateTimeOffset? Published { get; set; }
    /// <summary>The release's own page ("All changes").</summary>
    public string? PageUrl { get; set; }
    public IReadOnlyList<ReleaseAsset> Assets { get; set; } = Array.Empty<ReleaseAsset>();

    public string? SetupUrl { get; set; }
    public long SetupSize { get; set; }
    public string? ToolsUrl { get; set; }
    public long ToolsSize { get; set; }
    /// <summary>((Triforce) the tools in parts, when the release has them: ToolsInstaller.)</summary>
    public string? ToolsCodeUrl { get; set; }
    public long ToolsCodeSize { get; set; }
    public string? ToolsBaseUrl { get; set; }
    public long ToolsBaseSize { get; set; }
    /// <summary>Linux: triforce-linux.txt when the release carries one (none does today: LinuxUpdate).</summary>
    public string? LinuxListUrl { get; set; }
    /// <summary>Linux: the AppImage (Triforce-&lt;tag&gt;[-linux-test]-x86_64.AppImage).</summary>
    public string? LinuxAppImageUrl { get; set; }
    public long LinuxAppImageSize { get; set; }

    /// <summary>The first three "New in" items of the notes (ReleaseNotes.NewIn).</summary>
    public IReadOnlyList<NewInItem> NewIn => ReleaseNotes.NewIn(Body, 3);

    /// <summary>The release list's answer, read with a real JSON parser: a list of releases (the newest first) or one release.
    /// Each asset's object holds a nested "uploader" object, so nothing simpler than a parser reads it right (v0.1.0 found no
    /// assets). Asset names are matched without case.</summary>
    public static Release Parse(string json)
    {
        using var doc = JsonDocument.Parse(json);
        var root = doc.RootElement;
        var r = new Release();
        JsonElement release;
        if (root.ValueKind == JsonValueKind.Array)
        {
            if (root.GetArrayLength() == 0) return r;
            release = root[0];
        }
        else release = root;
        if (release.ValueKind != JsonValueKind.Object) return r;
        static string? Str(JsonElement o, string name) => o.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;
        static long Num(JsonElement o, string name) => o.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt64(out var n) ? n : 0;
        r.Tag = Str(release, "tag_name") ?? "";
        r.Name = Str(release, "name") ?? "";
        r.Body = Str(release, "body") ?? "";
        r.PageUrl = Str(release, "html_url");
        r.Prerelease = release.TryGetProperty("prerelease", out var pre) && pre.ValueKind == JsonValueKind.True;
        if (Str(release, "published_at") is { } when && DateTimeOffset.TryParse(when, out var t)) r.Published = t;
        var assets = new List<ReleaseAsset>();
        if (release.TryGetProperty("assets", out var list) && list.ValueKind == JsonValueKind.Array)
            foreach (var f in list.EnumerateArray())
            {
                if (f.ValueKind != JsonValueKind.Object) continue;
                var name = Str(f, "name") ?? "";
                var link = Str(f, "browser_download_url");
                var size = Num(f, "size");
                assets.Add(new ReleaseAsset(name, link, size));
                bool Is(string n) => string.Equals(name, n, StringComparison.OrdinalIgnoreCase);
                if (Is(BuildInfo.SetupAsset)) { r.SetupUrl = link; r.SetupSize = size; }
                if (Is(BuildInfo.ToolsAsset)) { r.ToolsUrl = link; r.ToolsSize = size; }
                if (Is(BuildInfo.ToolsCodeAsset)) { r.ToolsCodeUrl = link; r.ToolsCodeSize = size; }
                if (Is(BuildInfo.ToolsBaseAsset)) { r.ToolsBaseUrl = link; r.ToolsBaseSize = size; }
                if (Is(BuildInfo.LinuxListAsset)) r.LinuxListUrl = link;
                if (name.EndsWith("-x86_64.AppImage", StringComparison.OrdinalIgnoreCase) && name.StartsWith("Triforce", StringComparison.OrdinalIgnoreCase))
                {
                    r.LinuxAppImageUrl = link;
                    r.LinuxAppImageSize = size;
                }
            }
        r.Assets = assets;
        return r;
    }

    /// <summary>The address asked: TRIFORCE_RELEASE_API, else the release list.</summary>
    public static string ApiUrl(TriforceContext ctx) => ctx.Env("TRIFORCE_RELEASE_API") is { Length: > 0 } o ? o : BuildInfo.ReleaseApi;

    /// <summary>The latest release (20 s for the answer). 404, an empty list or an empty tag: NoReleaseException; another answer
    /// that is no success throws HttpRequestException.</summary>
    public static async Task<Release> LatestAsync(TriforceContext ctx, HttpClient http, CancellationToken token)
    {
        using var ask = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        using var both = CancellationTokenSource.CreateLinkedTokenSource(token, ask.Token);
        using var response = await http.GetAsync(ApiUrl(ctx), both.Token).ConfigureAwait(false);
        if (response.StatusCode == HttpStatusCode.NotFound) throw new NoReleaseException();
        response.EnsureSuccessStatusCode();
        var r = Parse(await response.Content.ReadAsStringAsync(both.Token).ConfigureAwait(false));
        if (r.Tag.Length == 0) throw new NoReleaseException();   // (an empty list: nothing published)
        return r;
    }
}

/// <summary>The release notes' "New in" items.</summary>
public static class ReleaseNotes
{
    static readonly Regex BoldItem = new(@"^\s*(?:[-*+]\s+)?\*\*(?<t>.+?)\*\*\s*(?<r>.*)$", RegexOptions.Compiled);
    static readonly Regex Bullet = new(@"^\s*[-*+]\s+(?<b>.+)$", RegexOptions.Compiled);

    /// <summary>The first items of the notes: lines that are "**Title.** One line" (bulleted or not), the release rule DESIGN 7
    /// asks for; when the notes have fewer than that many, their first bullet lines instead (a bullet's own bold start is its
    /// title). Markdown is taken out of the words.</summary>
    public static IReadOnlyList<NewInItem> NewIn(string? notes, int count = 3)
    {
        if (string.IsNullOrWhiteSpace(notes)) return Array.Empty<NewInItem>();
        var lines = notes.Replace("\r", "").Split('\n');
        var bold = new List<NewInItem>();
        var bullets = new List<NewInItem>();
        foreach (var line in lines)
        {
            if (line.TrimStart().StartsWith('#') || line.TrimStart().StartsWith('>')) continue;
            if (BoldItem.Match(line) is { Success: true } m && bold.Count < count)
                bold.Add(new NewInItem(Title(m.Groups["t"].Value), Plain(m.Groups["r"].Value)));
            if (Bullet.Match(line) is { Success: true } b && bullets.Count < count)
            {
                var text = b.Groups["b"].Value;
                bullets.Add(BoldItem.Match(text) is { Success: true } bm ? new NewInItem(Title(bm.Groups["t"].Value), Plain(bm.Groups["r"].Value)) : new NewInItem(Plain(text), ""));
            }
        }
        if (bold.Count >= count) return bold;
        return bullets.Count > 0 ? bullets : bold;
    }

    static string Title(string t) => Plain(t).TrimEnd('.', ':', ' ');

    /// <summary>Markdown out: [text](link) -> text, `code` -> code, ** and __ gone, &amp;amp; and the like read.</summary>
    public static string Plain(string s)
    {
        s = Regex.Replace(s, @"\[([^\]]*)\]\([^)]*\)", "$1");
        s = s.Replace("**", "").Replace("__", "").Replace("`", "");
        s = WebUtility.HtmlDecode(s);
        return Regex.Replace(s, @"\s+", " ").Trim();
    }
}
