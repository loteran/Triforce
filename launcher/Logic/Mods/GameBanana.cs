// GameBanana's mods for this game (ported from the game's own Mods menu, gpu/vk/vk_mods.cpp; research/02 7.3):
//   the section    https://gamebanana.com/apiv11/Game/26242/Subfeed?_sSort=default&_csvModelInclusions=Mod,Wip&_nPage=<n>
//   a search       .../apiv11/Util/Search/Results?_sModelName=Mod&_idGameRow=26242&_sOrder=best_match&_nPage=<n>&_sSearchString=<q>
//                  (+ the WiPs' first page, _sModelName=Wip: GameBanana's search takes one kind a request), and when the
//                  whole-word search finds nothing, the section's name filter: Subfeed...&_nPage=<n>&_sName=<q> (3 letters at least)
//   the details    .../apiv11/<Mod|Wip>/<id>/ProfilePage   (_aFiles[]: _idRow, _sFile, _sDescription, _sDownloadUrl, _nFilesize,
//                  _tsDateAdded; _sText the description as HTML; _nDownloadCount)
// Mods and WiPs: a WiP with files is often a mod all the same, and the online co-op kit is posted as one. A WiP with nothing to
// download (_bHasFiles false) is left out; a record of another game (_aGame._idRow) too. A page that fails with cards already
// listed is tried again after 2, 4 and 8 s (the first page's failure is shown at once, as in the game).
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;

namespace Triforce.Logic;

/// <summary>A card: one mod or WiP of the section.</summary>
public sealed record GbMod(int Id, bool Wip, string Name, string Author, string Category, string? Page, int Likes, int Views, long Updated, string? ThumbUrl)
{
    /// <summary>A program of its own (Kits): installed into kits\&lt;folder&gt; and started, not put in mods.</summary>
    public bool IsKit => Kits.Find(Wip, Id) != null;
}

public sealed record GbFile(int Id, string Name, string Description, string Url, long Bytes, long Added);

public sealed record GbDetails(int Id, bool Wip, string Text, int Downloads, IReadOnlyList<GbFile> Files)
{
    /// <summary>The file to offer first: a kit's Windows file (Squadala posts a -windows and a -linux zip), else the first.</summary>
    public int DefaultFile(bool isKit)
    {
        if (isKit)
            for (var k = 0; k < Files.Count; k++)
                if (Files[k].Name.Contains(OperatingSystem.IsWindows() ? "windows" : "linux", StringComparison.OrdinalIgnoreCase)) return k;
        return 0;
    }
}

public sealed record GbPage(IReadOnlyList<GbMod> Mods, int Page, int Total, bool Complete, bool NameFilter);

public sealed class GameBanana
{
    /// <summary>GameBanana's game: The Legend of Zelda: Ocarina of Time (Switch 2).</summary>
    public const int GameId = 26242;
    const string Api = "https://gamebanana.com/apiv11";

    readonly HttpClient http;
    public GameBanana(HttpClient? http = null)
    {
        this.http = http ?? Create();
    }

    /// <summary>The waits before a failed page is asked again (old: 2, 4, 8 s).</summary>
    public TimeSpan[] RetryDelays { get; set; } = { TimeSpan.FromSeconds(2), TimeSpan.FromSeconds(4), TimeSpan.FromSeconds(8) };

    public static HttpClient Create()
    {
        // (GameBanana's /dl/ links send to its file server: redirects followed. 15 s to connect, 60 s for an answer as WinHTTP had)
        var h = new HttpClient(new SocketsHttpHandler { AllowAutoRedirect = true, ConnectTimeout = TimeSpan.FromSeconds(15), AutomaticDecompression = System.Net.DecompressionMethods.All })
        { Timeout = Timeout.InfiniteTimeSpan };
        h.DefaultRequestHeaders.UserAgent.ParseAdd("Triforce-mod-browser/" + BuildInfo.Version);
        h.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));
        h.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("*/*", 0.8));
        return h;
    }

    public static string ListUrl(int page) => Api + "/Game/" + GameId + "/Subfeed?_sSort=default&_csvModelInclusions=Mod,Wip&_nPage=" + page;
    public static string SearchUrl(bool wip, int page, string query) =>
        Api + "/Util/Search/Results?_sModelName=" + (wip ? "Wip" : "Mod") + "&_idGameRow=" + GameId + "&_sOrder=best_match&_nPage=" + page + "&_sSearchString=" + UrlEncode(query);
    public static string NameFilterUrl(int page, string query) => ListUrl(page) + "&_sName=" + UrlEncode(query);
    public static string ProfileUrl(bool wip, int id) => Api + "/" + (wip ? "Wip" : "Mod") + "/" + id + "/ProfilePage";

    /// <summary>A search's text in a URL: its UTF-8 bytes, letters, digits and - _ . ~ as they are, the rest %XX (sent in another
    /// code page GameBanana drops every filter, and &amp; or + cut or change the query).</summary>
    public static string UrlEncode(string s)
    {
        var o = new StringBuilder();
        foreach (var b in Encoding.UTF8.GetBytes(s))
        {
            var c = (char)b;
            if (c is >= '0' and <= '9' or >= 'A' and <= 'Z' or >= 'a' and <= 'z' or '-' or '_' or '.' or '~') o.Append(c);
            else o.Append('%').Append(b.ToString("X2"));
        }
        return o.ToString();
    }

    async Task<string> GetTextAsync(string url, CancellationToken token)
    {
        using var r = await http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, token).ConfigureAwait(false);
        if (!r.IsSuccessStatusCode) throw new HttpRequestException("GameBanana answered " + (int)r.StatusCode, null, r.StatusCode);
        return await r.Content.ReadAsStringAsync(token).ConfigureAwait(false);
    }

    async Task<string> GetWithRetriesAsync(string url, bool retry, CancellationToken token)
    {
        for (var attempt = 0; ; attempt++)
        {
            try { return await GetTextAsync(url, token).ConfigureAwait(false); }
            catch (Exception) when (retry && attempt < RetryDelays.Length && !token.IsCancellationRequested)
            {
                await Task.Delay(RetryDelays[attempt], token).ConfigureAwait(false);
            }
        }
    }

    /// <summary>One page of the section (15 records a page), newest activity first.</summary>
    public async Task<GbPage> ListAsync(int page, CancellationToken token = default) =>
        ParsePage(await GetWithRetriesAsync(ListUrl(page), page > 1, token).ConfigureAwait(false), null, page, false);

    /// <summary>A search: GameBanana's own (whole words, best matches first, the WiPs' first page with the mods' first); when that
    /// finds nothing on its first page and the text has 3 letters or more, the section's name filter (a part of a name: "majora",
    /// "zeld"). nameFilter: ask the name filter's page directly (the next pages of a search that went there).</summary>
    public async Task<GbPage> SearchAsync(string query, int page, bool nameFilter = false, CancellationToken token = default)
    {
        query = query.Trim();
        if (nameFilter) return ParsePage(await GetWithRetriesAsync(NameFilterUrl(page, query), page > 1, token).ConfigureAwait(false), null, page, true);
        var mods = await GetWithRetriesAsync(SearchUrl(false, page, query), page > 1, token).ConfigureAwait(false);
        string? wips = null;
        if (page == 1)
            try { wips = await GetTextAsync(SearchUrl(true, 1, query), token).ConfigureAwait(false); }
            catch (Exception) when (!token.IsCancellationRequested) { }   // (the WiPs could not be read: the mods alone)
        var got = ParsePage(mods, wips, page, false);
        if (page == 1 && got.Mods.Count == 0 && got.Complete && query.Length >= 3)
            return ParsePage(await GetWithRetriesAsync(NameFilterUrl(1, query), false, token).ConfigureAwait(false), null, 1, true);
        return got;
    }

    /// <summary>A page's records as cards: mods and WiPs only, a WiP with nothing to download left out, another game's records
    /// left out, the same mod once. Total and Complete from _aMetadata.</summary>
    public static GbPage ParsePage(string json, string? wipJson, int page, bool nameFilter)
    {
        using var doc = JsonDocument.Parse(json);
        using var wdoc = wipJson != null ? TryParse(wipJson) : null;
        var j = doc.RootElement;
        var meta = Get(j, "_aMetadata");
        var total = (int)Num(meta, "_nRecordCount") + (wdoc != null ? (int)Num(Get(wdoc.RootElement, "_aMetadata"), "_nRecordCount") : 0);
        var complete = meta.ValueKind == JsonValueKind.Object && meta.TryGetProperty("_bIsComplete", out var c) && c.ValueKind == JsonValueKind.True;
        var list = new List<GbMod>();
        var records = new List<JsonElement>();
        if (Get(j, "_aRecords") is { ValueKind: JsonValueKind.Array } a) records.AddRange(a.EnumerateArray());
        if (wdoc != null && Get(wdoc.RootElement, "_aRecords") is { ValueKind: JsonValueKind.Array } w) records.AddRange(w.EnumerateArray());
        foreach (var r in records)
        {
            if (r.ValueKind != JsonValueKind.Object) continue;
            var model = Str(r, "_sModelName");
            if (model != "Mod" && model != "Wip") continue;   // (the section's requests, questions and tools: not mods)
            if (model == "Wip" && r.TryGetProperty("_bHasFiles", out var hf) && hf.ValueKind == JsonValueKind.False) continue;
            if (Get(r, "_aGame") is { ValueKind: JsonValueKind.Object } g && Num(g, "_idRow") != GameId) continue;
            var id = (int)Num(r, "_idRow");
            var wip = model == "Wip";
            if (list.Any(o => o.Id == id && o.Wip == wip)) continue;
            string? thumb = null;
            if (Get(Get(r, "_aPreviewMedia"), "_aImages") is { ValueKind: JsonValueKind.Array } imgs && imgs.GetArrayLength() > 0 && imgs[0] is { ValueKind: JsonValueKind.Object } img)
            {
                // (530 wide when GameBanana made that size: sharp in the details; the cards draw it smaller)
                var file = Str(img, "_sFile530") is { Length: > 0 } f530 ? f530 : Str(img, "_sFile220") is { Length: > 0 } f220 ? f220 : Str(img, "_sFile");
                thumb = Str(img, "_sBaseUrl") + "/" + file;
            }
            var updated = Num(r, "_tsDateModified") is var m && m != 0 ? m : Num(r, "_tsDateAdded");
            list.Add(new GbMod(id, wip, Str(r, "_sName"), Str(Get(r, "_aSubmitter"), "_sName"), Str(Get(r, "_aRootCategory"), "_sName"), Str(r, "_sProfileUrl") is { Length: > 0 } p ? p : null,
                (int)Num(r, "_nLikeCount"), (int)Num(r, "_nViewCount"), updated, thumb));
        }
        return new GbPage(list, page, total, complete, nameFilter);
    }

    static JsonDocument? TryParse(string s) { try { return JsonDocument.Parse(s); } catch (Exception) { return null; } }
    static JsonElement Get(JsonElement o, string name) => o.ValueKind == JsonValueKind.Object && o.TryGetProperty(name, out var v) ? v : default;
    static string Str(JsonElement o, string name) => Get(o, name) is var v && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : v.ValueKind == JsonValueKind.Number ? v.GetRawText() : "";
    static long Num(JsonElement o, string name) => Get(o, name) is var v && v.ValueKind == JsonValueKind.Number ? (v.TryGetInt64(out var n) ? n : (long)v.GetDouble())
        : v.ValueKind == JsonValueKind.String && long.TryParse(v.GetString(), out var s) ? s : 0;

    /// <summary>A mod's details: its files (those with a download link), its description as plain text, its downloads.</summary>
    public async Task<GbDetails> DetailsAsync(int id, bool wip, CancellationToken token = default)
    {
        using var doc = JsonDocument.Parse(await GetTextAsync(ProfileUrl(wip, id), token).ConfigureAwait(false));
        return ParseDetails(doc.RootElement, id, wip);
    }

    public static GbDetails ParseDetails(JsonElement j, int id, bool wip)
    {
        var files = new List<GbFile>();
        if (Get(j, "_aFiles") is { ValueKind: JsonValueKind.Array } a)
            foreach (var f in a.EnumerateArray())
            {
                var url = Str(f, "_sDownloadUrl");
                if (url.Length > 0) files.Add(new GbFile((int)Num(f, "_idRow"), Str(f, "_sFile"), Str(f, "_sDescription"), url, Num(f, "_nFilesize"), Num(f, "_tsDateAdded")));
            }
        return new GbDetails(id, wip, Plain(Str(j, "_sText")), (int)Num(j, "_nDownloadCount"), files);
    }

    /// <summary>A card's picture's bytes (at most 8 MB); the App decodes it.</summary>
    public async Task<byte[]> ThumbnailAsync(string url, CancellationToken token = default)
    {
        if (!url.StartsWith("https://", StringComparison.Ordinal)) throw new ArgumentException("not an https link");
        using var r = await http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, token).ConfigureAwait(false);
        r.EnsureSuccessStatusCode();
        await using var s = await r.Content.ReadAsStreamAsync(token).ConfigureAwait(false);
        using var m = new MemoryStream();
        var buf = new byte[1 << 16];
        for (int n; (n = await s.ReadAsync(buf, token).ConfigureAwait(false)) > 0;)
        {
            m.Write(buf, 0, n);
            if (m.Length > 8 << 20) throw new InvalidDataException("the picture is larger than 8 MB");
        }
        return m.ToArray();
    }

    /// <summary>GameBanana's descriptions are HTML: the text alone, a line where a paragraph or a break was (vk_mods.cpp plain).</summary>
    public static string Plain(string html)
    {
        var o = new StringBuilder();
        string[][] ents =
        {
            new[] { "&amp;", "&" }, new[] { "&lt;", "<" }, new[] { "&gt;", ">" }, new[] { "&quot;", "\"" }, new[] { "&#39;", "'" }, new[] { "&apos;", "'" },
            new[] { "&nbsp;", " " }, new[] { "&rsquo;", "’" }, new[] { "&lsquo;", "‘" }, new[] { "&ldquo;", "“" }, new[] { "&rdquo;", "”" },
            new[] { "&hellip;", "..." }, new[] { "&mdash;", "—" }, new[] { "&ndash;", "–" },
        };
        for (var i = 0; i < html.Length;)
        {
            if (html[i] == '<')
            {
                var end = html.IndexOf('>', i);
                if (end < 0) break;
                var tag = html.Substring(i + 1, end - i - 1).ToLowerInvariant();
                if (tag.StartsWith("br") || tag.StartsWith("/p") || tag.StartsWith("/li") || tag.StartsWith("/h") || tag.StartsWith("/div")) o.Append('\n');
                else if (tag.StartsWith("li")) o.Append("- ");
                i = end + 1;
                continue;
            }
            if (html[i] == '&')
            {
                var hit = ents.FirstOrDefault(e => string.CompareOrdinal(html, i, e[0], 0, e[0].Length) == 0);
                if (hit != null) { o.Append(hit[1]); i += hit[0].Length; continue; }
            }
            o.Append(html[i++]);
        }
        // (runs of blank lines and spaces: one each)
        var tidy = new StringBuilder();
        var newlines = 0;
        var space = false;
        foreach (var ch in o.ToString())
        {
            var c = ch == '\r' || ch == '\t' ? ' ' : ch;
            if (c == '\n') { if (++newlines <= 2 && tidy.Length > 0) tidy.Append('\n'); space = false; continue; }
            if (c == ' ') { if (!space && tidy.Length > 0 && tidy[^1] != '\n') tidy.Append(' '); space = true; continue; }
            newlines = 0;
            space = false;
            tidy.Append(c);
        }
        var s = tidy.ToString().TrimEnd('\n', ' ');
        return s.Length > 1400 ? s.Substring(0, 1400) + "..." : s;
    }
}
