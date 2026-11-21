using System.Collections.Concurrent;
using System.Text.Json;
using System.Text.Json.Nodes;
using Triforce.Logic;

namespace Triforce.Launcher;

/// <summary>The Mods page (DESIGN 6 Mods, research/02 7): Browse = GameBanana's section for this game (public read calls only,
/// and the downloads the player asks for), Yours = the game folder's mods folder as the game finds it (ModManager). Installing,
/// updating, switching on and off and removing; a kit (Kits: Squadala only) is installed into kits\ and started. Code patches
/// are built into the game, so a change of those says the game must be built again, and how long that takes.</summary>
public sealed partial class Backend
{
    readonly Lazy<HttpClient> gbHttp = new(GameBanana.Create);
    GameBanana? gbApi;
    GameBanana Gb => gbApi ??= new GameBanana(gbHttp.Value);

    readonly object modsGate = new();
    string modsTab = "browse", modsQuery = "";
    int modsPage, modsGen;
    bool modsNameFilter, modsMore, modsLoaded;
    string? modsError;
    List<GbMod> gbCards = new();
    readonly ConcurrentDictionary<string, GbDetails> gbDetails = new();
    /// <summary>What is being worked on: the item's id and the line under its name meanwhile.</summary>
    readonly ConcurrentDictionary<string, string> modsBusy = new();
    string? rebuildTime;

    partial void RegisterMods()
    {
        OnAsync("mods.list", m => ModsList((string?)m["tab"] ?? "browse", (string?)m["q"] ?? ""));
        OnAsync("mods.more", _ => ModsMore());
        OnAsync("mods.main", m => ModsMain((string?)m["id"]));
        OnAsync("mods.file", m => ModsFileChosen((string?)m["id"], (string?)m["ctx"]));
        On("mods.remove", m => ModsRemove((string?)m["id"]));
        On("mods.page", m =>
        {
            var id = (string?)m["id"] ?? "";
            var url = CardOf(id)?.Page ?? (GameDir is { } g ? MineMods(g, false).FirstOrDefault(x => "m:" + x.Rel == id)?.Info?.Page : null) ?? KitInfo(id)?.Page;
            if (url != null) OpenLink(url);
        });
    }

    static string CardKey(GbMod m) => "gb:" + (m.Wip ? "w" : "m") + ":" + m.Id;
    GbMod? CardOf(string id) { lock (modsGate) return gbCards.FirstOrDefault(c => CardKey(c) == id); }

    // ---------------- lists ----------------

    async Task ModsList(string tab, string q)
    {
        modsTab = tab == "mine" ? "mine" : "browse";
        if (modsTab == "mine") { SendMods(); return; }
        int gen;
        lock (modsGate) { gen = ++modsGen; modsQuery = q.Trim(); modsError = null; }
        try
        {
            var page = modsQuery.Length == 0 ? await Gb.ListAsync(1) : await Gb.SearchAsync(modsQuery, 1);
            lock (modsGate)
            {
                if (gen != modsGen) return;
                gbCards = page.Mods.ToList();
                modsPage = 1; modsNameFilter = page.NameFilter; modsMore = !page.Complete && page.Mods.Count > 0; modsLoaded = true;
            }
        }
        catch (Exception e) when (e is HttpRequestException || e is TaskCanceledException || e is JsonException || e is IOException)
        {
            lock (modsGate)
            {
                if (gen != modsGen) return;
                gbCards = new(); modsMore = false; modsLoaded = false;
                modsError = "GameBanana could not be reached (" + e.Message + "). Check this PC's connection and open the page again.";
            }
            Log.Write("mods: " + e.Message);
        }
        if (modsTab != "browse") return;   // (the player went to Yours meanwhile: the cards wait for Browse)
        SendMods();
        await FetchDetails(gen);
    }

    async Task ModsMore()
    {
        int gen, next; string q; bool nameFilter;
        lock (modsGate) { if (!modsMore) return; gen = modsGen; next = modsPage + 1; q = modsQuery; nameFilter = modsNameFilter; }
        try
        {
            var page = q.Length == 0 ? await Gb.ListAsync(next) : await Gb.SearchAsync(q, next, nameFilter);
            lock (modsGate)
            {
                if (gen != modsGen) return;
                var have = new HashSet<string>(gbCards.Select(CardKey));
                gbCards = gbCards.Concat(page.Mods.Where(m => have.Add(CardKey(m)))).ToList();
                modsPage = next; modsMore = !page.Complete && page.Mods.Count > 0;
            }
        }
        catch (Exception e) when (e is HttpRequestException || e is TaskCanceledException || e is JsonException || e is IOException)
        {
            Toast("GameBanana could not be reached (" + H(e.Message) + ").", "error");
            return;
        }
        SendMods();
        await FetchDetails(gen);
    }

    /// <summary>Each card's details (its description and files: what a sheet shows, and whether an installed one has a newer
    /// file), four at a time, then the list again.</summary>
    async Task FetchDetails(int gen)
    {
        List<GbMod> cards;
        lock (modsGate) cards = gbCards.Where(c => !gbDetails.ContainsKey(CardKey(c))).ToList();
        if (cards.Count == 0) return;
        using var gate = new SemaphoreSlim(4);
        await Task.WhenAll(cards.Select(async c =>
        {
            await gate.WaitAsync();
            try { gbDetails[CardKey(c)] = await Gb.DetailsAsync(c.Id, c.Wip); }
            catch (Exception e) { Log.Write("mods: details of " + c.Id + ": " + e.Message); }
            finally { gate.Release(); }
        }));
        if (gen == modsGen && modsTab == "browse") SendMods();
    }

    void SendMods()
    {
        JsonObject state;
        try { state = ModsState(); }
        catch (Exception e) { Log.Write("mods: " + e); state = new JsonObject { ["tab"] = modsTab, ["items"] = new JsonArray(), ["more"] = false, ["loaded"] = true, ["note"] = H(e.Message) }; }
        SendState(new JsonObject { ["mods"] = state });
    }

    List<Mod> MineMods(string game, bool carried)
    {
        try { return new ModManager(ctx).ListGame(game, carried); } catch (Exception e) { Log.Write("mods: " + e.Message); return new(); }
    }

    JsonObject ModsState()
    {
        var game = GameDir;
        var items = new JsonArray();
        string note = "";
        var tab = modsTab;
        bool more, loaded;
        if (tab == "browse")
        {
            List<GbMod> cards;
            lock (modsGate) { cards = gbCards.ToList(); more = modsMore; loaded = modsLoaded; if (modsError != null) note = H(modsError); }
            var inst = game != null ? new GameBananaInstaller(ctx).Installed(game) : Array.Empty<InstalledMod>();
            var local = game != null ? MineMods(game, false) : new List<Mod>();
            foreach (var c in cards)
            {
                var key = CardKey(c);
                gbDetails.TryGetValue(key, out var d);
                var kit = Kits.Find(c.Wip, c.Id) != null;
                var hit = inst.FirstOrDefault(i => i.Id == c.Id && i.Wip == c.Wip);
                string state;
                if (hit == null && d != null && d.Files.Count == 0) state = "page";   // (nothing to download: its page only, no button)
                else if (kit) state = hit != null ? "start" : "kit";
                else if (hit == null) state = "get";
                else if (d != null && game != null && new GameBananaInstaller(ctx).State(game, c, d).Update) state = "update";
                else state = local.Any(m => Ours(m, c) && m.On) ? "on" : "off";
                var sub = string.Join(" · ", new[] { c.Author, c.Category, c.Wip ? "work in progress" : "" }.Where(s => s.Length > 0));
                if (modsBusy.TryGetValue(key, out var busy)) { state = "busy"; sub = busy; }
                items.Add(new JsonObject
                {
                    ["id"] = key, ["name"] = c.Name, ["sub"] = sub, ["state"] = state,
                    ["img"] = c.ThumbUrl is { } t && t.StartsWith("https://", StringComparison.Ordinal) ? t : null,
                    ["desc"] = d != null ? Describe(d, kit) : null,
                    ["page"] = c.Page is { } p && p.StartsWith("https://", StringComparison.Ordinal) ? p : null,
                    ["removable"] = !kit && hit != null && state != "busy",
                });
            }
            if (loaded && cards.Count == 0 && note.Length == 0)
                note = modsQuery.Length > 0 ? "Nothing on GameBanana matches “" + H(modsQuery) + "”." : "GameBanana lists no mods for this game right now.";
        }
        else
        {
            more = false; loaded = true;
            if (game != null)
            {
                foreach (var m in MineMods(game, true))
                {
                    var key = "m:" + m.Rel;
                    var parts = new List<string>();
                    if (m.Info?.Author is { Length: > 0 } a) parts.Add(a);
                    parts.Add(Mods.Carries(m));
                    if (m.OffAbove != null) parts.Add("off with its folder " + m.OffAbove);
                    var state = m.On ? "on" : "off";
                    var sub = string.Join(" · ", parts);
                    if (modsBusy.TryGetValue(key, out var busy)) { state = "busy"; sub = busy; }
                    items.Add(new JsonObject
                    {
                        ["id"] = key, ["name"] = m.Info?.Name is { Length: > 0 } n ? n : m.Name, ["sub"] = sub, ["state"] = state, ["img"] = ThumbOf(m),
                        ["desc"] = "Its folder: mods" + Path.DirectorySeparatorChar + m.Rel.Replace('/', Path.DirectorySeparatorChar) + ". " +
                                   (m.Built ? "It changes the game's code, so the game is built again for it." : "Its files are read when the game starts."),
                        ["page"] = m.Info?.Page is { } p && p.StartsWith("https://", StringComparison.Ordinal) ? p : null,
                        ["removable"] = state != "busy",
                    });
                }
                foreach (var k in Kits.All)
                {
                    var info = KitInfo("kit:" + k.Id);
                    if (info == null) continue;
                    var key = "gb:" + (k.Wip ? "w" : "m") + ":" + k.Id;
                    var busy = modsBusy.TryGetValue(key, out var b) ? b : null;
                    items.Add(new JsonObject
                    {
                        ["id"] = "kit:" + k.Id, ["name"] = info.Name, ["sub"] = busy ?? "A program of its own, in kits" + Path.DirectorySeparatorChar + k.Folder,
                        ["state"] = busy != null ? "busy" : "start", ["img"] = null,
                        ["desc"] = "It runs beside the game in a window of its own. Start it, then follow its steps there.",
                        ["page"] = info.Page, ["removable"] = false,
                    });
                }
                if (items.Count == 0) note = "There are no mods in your mods folder yet. Install one from Browse, or put a mod's folder into " + H(Mods.FolderOf(game)) + ".";
            }
        }
        if (game == null) note = "Build the game first: mods go into its mods folder.";
        else if (SafeNeedsRebuild(game)) note = "Changes take effect after the game is built again: <b>" + H(RebuildTime()) + "</b>.";
        return new JsonObject { ["tab"] = tab, ["items"] = items, ["more"] = more, ["loaded"] = loaded, ["note"] = note };
    }

    /// <summary>A mod from GameBanana: its card's picture, when Browse has shown that card.</summary>
    string? ThumbOf(Mod m)
    {
        if (m.Info is not { GameBananaId: > 0 }) return null;
        lock (modsGate) return gbCards.FirstOrDefault(c => Ours(m, c))?.ThumbUrl is { } t && t.StartsWith("https://", StringComparison.Ordinal) ? t : null;
    }

    static bool Ours(Mod m, GbMod c) => m.Info is { } i && i.GameBananaId == c.Id && i.Wip == c.Wip;

    bool SafeNeedsRebuild(string game)
    {
        try { return new ModManager(ctx).NeedsRebuild(game); } catch (Exception) { return false; }
    }

    /// <summary>"about 12 minutes" (this PC's last update build) or a stated range, as the home says it.</summary>
    string RebuildTime()
    {
        if (rebuildTime != null) return rebuildTime;
        try
        {
            var est = BuildEstimator.Estimate(BuildHistory.Load(ctx.WorkFolder), true, Environment.ProcessorCount, Gpus.Main()?.Vendor, ctx.IsWindows);
            var t = est.Text;
            rebuildTime = t.StartsWith("About ", StringComparison.Ordinal) ? "about " + t.Substring(6) : t;
        }
        catch (Exception) { rebuildTime = "about 5 to 60 minutes"; }
        return rebuildTime;
    }

    static string Describe(GbDetails d, bool kit)
    {
        var text = d.Text.Trim();
        var files = d.Files.Count == 0 ? "No file to download." : d.Files.Count == 1 ? "One file, " + Size(d.Files[0].Bytes) + "." : d.Files.Count + " files to choose from.";
        if (kit) files += " It is installed beside the game and started in a window of its own.";
        return (text.Length > 0 ? text + "\n\n" : "") + files;
    }

    static string Size(long b) => b >= 1L << 30 ? (b / (double)(1L << 30)).ToString("0.0", System.Globalization.CultureInfo.InvariantCulture) + " GB"
        : b >= 1L << 20 ? Math.Max(1, (int)Math.Round(b / (double)(1L << 20))) + " MB" : Math.Max(1, (int)Math.Round(b / 1024.0)) + " KB";

    sealed record KitFile(string Name, string? Page);

    /// <summary>An installed kit's triforce_kit.json: its name and page (null when that kit is not installed).</summary>
    KitFile? KitInfo(string id)
    {
        if (GameDir is not { } g || !id.StartsWith("kit:", StringComparison.Ordinal) || !int.TryParse(id.AsSpan(4), out var n)) return null;
        var k = Kits.All.FirstOrDefault(x => x.Id == n);
        if (k == null) return null;
        try
        {
            var file = Path.Combine(g, "kits", k.Folder, "triforce_kit.json");
            if (!File.Exists(file)) return null;
            using var doc = JsonDocument.Parse(File.ReadAllText(file));
            var r = doc.RootElement;
            string? S(string p) => r.TryGetProperty(p, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;
            var page = S("page");
            return new KitFile(S("name") is { Length: > 0 } nm ? nm : k.Folder, page != null && page.StartsWith("https://", StringComparison.Ordinal) ? page : null);
        }
        catch (Exception) { return null; }
    }

    // ---------------- changes ----------------

    async Task ModsMain(string? id)
    {
        if (id == null) return;
        if (GameDir is not { } game) { Toast("Build the game first: mods go into its mods folder.", "error"); return; }
        if (modsBusy.ContainsKey(id)) return;
        if (id.StartsWith("m:", StringComparison.Ordinal))
        {
            var mod = MineMods(game, true).FirstOrDefault(m => "m:" + m.Rel == id);
            if (mod == null) { Toast("That mod is no longer in the mods folder.", "error"); SendMods(); return; }
            Switch(game, new[] { mod }, !mod.On);
            return;
        }
        GbMod? card;
        if (id.StartsWith("kit:", StringComparison.Ordinal))
        {
            var k = int.TryParse(id.AsSpan(4), out var n) ? Kits.All.FirstOrDefault(x => x.Id == n) : null;
            if (k == null) return;
            card = new GbMod(k.Id, k.Wip, KitInfo(id)?.Name ?? k.Folder, "", "", KitInfo(id)?.Page, 0, 0, 0, null);
        }
        else card = CardOf(id);
        if (card == null) return;
        var key = CardKey(card);
        var kit = Kits.Find(card.Wip, card.Id) != null;
        if (!kit)
        {
            // (installed and current: the main button switches it)
            var hit = new GameBananaInstaller(ctx).Installed(game).FirstOrDefault(i => i.Id == card.Id && i.Wip == card.Wip);
            gbDetails.TryGetValue(key, out var known);
            if (hit != null && !(known != null && new GameBananaInstaller(ctx).State(game, card, known).Update))
            {
                var folders = MineMods(game, true).Where(m => Ours(m, card)).ToList();
                var anyOn = folders.Any(m => m.On);
                Switch(game, anyOn ? folders.Where(m => m.On).ToList() : folders.Take(1).ToList(), !anyOn);
                return;
            }
        }
        GbDetails d;
        try { d = gbDetails.TryGetValue(key, out var have) ? have : gbDetails[key] = await Gb.DetailsAsync(card.Id, card.Wip); }
        catch (Exception e) when (e is HttpRequestException || e is TaskCanceledException || e is JsonException)
        {
            Toast("GameBanana could not be reached (" + H(e.Message) + ").", "error");
            return;
        }
        if (d.Files.Count == 0) { Toast("This one has no file to download on GameBanana.", "error"); return; }
        if (!kit && d.Files.Count > 1)
        {
            var first = d.DefaultFile(false);
            Choice("Which file?", "<b>" + H(card.Name) + "</b> comes as " + d.Files.Count + " files. Most players want just one.", "mods.file",
                d.Files.Select((f, i) => (i.ToString(), f.Name, (string?)(Size(f.Bytes) + (f.Description.Length > 0 ? " · " + f.Description : "")), i == first)), "Install", key);
            return;
        }
        await Install(game, card, d.Files[d.DefaultFile(kit)]);
    }

    async Task ModsFileChosen(string? id, string? key)
    {
        if (id == null || key == null || GameDir is not { } game || !int.TryParse(id, out var i)) return;
        var card = CardOf(key);
        if (card == null || !gbDetails.TryGetValue(key, out var d) || i < 0 || i >= d.Files.Count) return;
        await Install(game, card, d.Files[i]);
    }

    async Task Install(string game, GbMod card, GbFile file)
    {
        var key = CardKey(card);
        if (!modsBusy.TryAdd(key, "Downloading " + Size(file.Bytes))) return;
        SendMods();
        var last = DateTime.MinValue;
        void Progress(DownloadProgress p)
        {
            if ((DateTime.UtcNow - last).TotalMilliseconds < 700) return;
            last = DateTime.UtcNow;
            modsBusy[key] = p.Fraction >= 1 ? "Unpacking" : "Downloading, " + (int)(p.Fraction * 100) + " % of " + Size(p.Total > 0 ? p.Total : file.Bytes);
            SendMods();
        }
        try
        {
            var msg = await new GameBananaInstaller(ctx).InstallAsync(gbHttp.Value, card, file, game, Progress, CancellationToken.None);
            Toast(H(msg), "success");
        }
        catch (Exception e) when (e is ModsException || e is InvalidOperationException || e is HttpRequestException || e is IOException || e is UnauthorizedAccessException || e is TaskCanceledException)
        {
            Log.Write("mods: install " + card.Id + ": " + e);
            Toast(H(e.Message), "error");
        }
        finally
        {
            modsBusy.TryRemove(key, out _);
            SendMods();
        }
    }

    void Switch(string game, IReadOnlyList<Mod> list, bool on)
    {
        if (list.Count == 0) return;
        var mm = new ModManager(ctx);
        try
        {
            foreach (var m in list) mm.SetOn(Mods.FolderOf(game), m, on);
            var name = list[0].Info?.Name is { Length: > 0 } n ? n : list[0].Name;
            Toast("<b>" + H(name) + "</b> is " + (on ? "on" : "off") + ".", "success");
        }
        catch (Exception e) when (e is ModsException || e is InvalidOperationException || e is IOException)
        {
            Toast(H(e.Message), "error");
        }
        SendMods();
    }

    void ModsRemove(string? id)
    {
        if (id == null || GameDir is not { } game) return;
        var all = MineMods(game, true);
        List<Mod> list;
        if (id.StartsWith("m:", StringComparison.Ordinal)) list = all.Where(m => "m:" + m.Rel == id).ToList();
        else if (CardOf(id) is { } card) list = all.Where(m => Ours(m, card)).ToList();
        else return;
        if (list.Count == 0) { Toast("That mod is no longer in the mods folder.", "error"); SendMods(); return; }
        var mm = new ModManager(ctx);
        try
        {
            // (deepest first: a mod inside another's folder goes before the folder it is in)
            foreach (var m in list.OrderByDescending(m => m.Rel.Length)) mm.Remove(Mods.FolderOf(game), m);
            var name = list[0].Info?.Name is { Length: > 0 } n ? n : list[0].Name;
            Toast("<b>" + H(name) + "</b> is removed. Its folder is kept in mods" + Path.DirectorySeparatorChar + Mods.Removed + ", should you want it back.", "success");
        }
        catch (Exception e) when (e is ModsException || e is InvalidOperationException || e is IOException)
        {
            Toast(H(e.Message), "error");
        }
        SendMods();
    }
}
