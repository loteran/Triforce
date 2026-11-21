using System.Collections.Concurrent;
using System.Text.Json.Nodes;
using Triforce.Logic;

namespace Triforce.Launcher;

/// <summary>The Saves page (DESIGN 6 Saves, research/02 6): the six files of the built game, and bringing saves in from an
/// emulator or a folder, or copying them out, each as two steps over the page (choose where, then a plain question that
/// names what is replaced and that it is backed up first). Refused while a game or emulator runs, with that reason.</summary>
public sealed partial class Backend
{
    /// <summary>Save sets waiting for the player's answer, by the key the question carries.</summary>
    readonly ConcurrentDictionary<string, SaveSet> savesPending = new();
    /// <summary>The emulators' saves found when a flow was opened (the choice's ids are indexes into it).</summary>
    List<SaveSet> savesFound = new();

    partial void RegisterSaves()
    {
        On("saves.list", _ => SendSaves());
        On("saves.import", _ => SavesFlow(true));
        On("saves.export", _ => SavesFlow(false));
        On("saves.from", m => SavesChosen(true, (string?)m["id"]));
        On("saves.to", m => SavesChosen(false, (string?)m["id"]));
        OnAsync("saves.confirm", m => SavesConfirm((string?)m["id"], (string?)m["ctx"]));
        On("saves.open", _ =>
        {
            if (GameDir is not { } g) { Toast("There is no game folder yet. Build the game first."); return; }
            var s = new Saves(ctx);
            var folder = s.TriforceFolder(g);
            OpenPlace(Directory.Exists(folder) ? folder : g);
        });
        On("saves.backups", _ =>
        {
            if (!Directory.Exists(ctx.SaveBackupsFolder)) { Toast("Nothing has been backed up yet. A copy of your saves is made there before anything replaces them."); return; }
            OpenPlace(ctx.SaveBackupsFolder);
        });
    }

    // ---------------- the page's state ----------------

    void SendSaves() => SendState(new JsonObject { ["saves"] = SavesState() });

    JsonObject SavesState()
    {
        var game = GameDir;
        var items = new JsonArray();
        var saves = new Saves(ctx);
        var slots = game != null ? saves.Slots(game) : Saves.SlotsIn(Path.Combine(dataDir, "no-game-folder"));   // (six empty files)
        var newest = game != null ? saves.Newest(game) : null;
        foreach (var s in slots)
        {
            var label = "File " + s.FileNumber;
            if (!s.Used)
            {
                items.Add(new JsonObject { ["slot"] = s.FileNumber, ["label"] = label, ["empty"] = true });
                continue;
            }
            items.Add(new JsonObject
            {
                ["slot"] = s.FileNumber, ["label"] = label, ["place"] = null,
                ["when"] = s.Written is { } w ? DayWord(w) : "",
                ["time"] = "",
                ["meta"] = s.Written is { } w2 ? "Saved " + DayWord(w2) + ", " + w2.ToString("HH:mm") : "",
                ["pic"] = s.Caption != null ? SavePicUrl(s) : "",
                ["last"] = newest != null && newest.Index == s.Index,
                ["empty"] = false,
            });
        }
        var used = slots.Count(s => s.Used);
        var eb = game == null ? "No game folder yet" : used == 0 ? "No saves yet in your game folder" : "Six files in your game folder";
        List<SaveSet> found;
        try { found = saves.FindEmulatorSaves(); } catch (Exception e) { Log.Write("saves: " + e.Message); found = new(); }
        var line = "From an emulator on this PC, or a folder.";
        if (found.Count > 0)
            line += " Found: " + string.Join(", ", found.Take(3).Select(f => f.Name + " (" + Files(f.Slots) + ")")) + (found.Count > 3 ? " and " + (found.Count - 3) + " more" : "") + ".";
        return new JsonObject { ["eb"] = eb, ["found"] = line, ["items"] = items };
    }

    static string Files(int n) => n == 1 ? "1 file" : n + " files";

    static readonly string[] MonthNames = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

    /// <summary>"today", "yesterday", "16 Nov" (with the year when it is another year's).</summary>
    static string DayWord(DateTime t)
    {
        var d = (DateTime.Today - t.Date).Days;
        if (d == 0) return "today";
        if (d == 1) return "yesterday";
        return t.Day + " " + MonthNames[t.Month - 1] + (t.Year != DateTime.Today.Year ? " " + t.Year : "");
    }

    static string SetLine(SaveSet s) =>
        (s.Slots > 0 ? Files(s.Slots) : "no files") + (s.Last > DateTime.MinValue ? ", saved " + DayWord(s.Last) : "");

    // ---------------- bring in / copy out ----------------

    /// <summary>Step one: where from (in) or where to (out). Refused at once while a game or emulator runs.</summary>
    void SavesFlow(bool into)
    {
        var game = GameDir;
        if (game == null) { Toast(into ? "Build the game first: your saves go into its folder." : "There is no game folder yet, so there are no saves to copy out.", "error"); return; }
        var saves = new Saves(ctx);
        if (saves.RunningGame() is { } running) { Toast("Close " + H(running) + " first: it would write its own saves back over the copy when it closes.", "error"); return; }
        if (!into && !Saves.IsSaveSet(saves.TriforceNewest(game))) { Toast("Triforce has no saves yet to copy out.", "error"); return; }
        List<SaveSet> found;
        try { found = saves.FindEmulatorSaves(); } catch (Exception e) { Log.Write("saves: " + e.Message); found = new(); }
        savesFound = found;
        var options = found.Select((f, i) => ("s" + i, f.Name, (string?)SetLine(f), i == 0)).ToList();
        options.Add(("folder", "A folder…", "Choose it yourself", found.Count == 0));
        if (into)
            Choice("Bring saves in", found.Count > 0 ? "Where are the saves you want to play with?" : "No emulator saves were found on this PC. Choose the folder they are in.",
                "saves.from", options, "Next");
        else
            Choice("Copy saves out", "Where should Triforce's saves go?", "saves.to", options, "Next");
    }

    /// <summary>Step one answered: a found save set, or a folder to pick first.</summary>
    void SavesChosen(bool into, string? id)
    {
        if (id == null) return;
        if (id == "folder")
        {
            PickThen(true, into ? "Choose the folder with your saves" : "Choose where the saves go", path => SavesFolderPicked(into, path));
            return;
        }
        if (!id.StartsWith('s') || !int.TryParse(id.AsSpan(1), out var i) || i < 0 || i >= savesFound.Count) return;
        SavesAsk(into, savesFound[i]);
    }

    void SavesFolderPicked(bool into, string path)
    {
        if (Saves.FindIn(path) is { } found)
        {
            SavesAsk(into, Saves.Describe(Path.GetFileName(found.TrimEnd('\\', '/')) is { Length: > 0 } n ? "the folder " + n : "that folder", found));
            return;
        }
        if (into) { Toast("There are no saves of this game in that folder.", "error"); return; }
        // (out: an empty folder takes the saves as they are; anything else gets a folder of their own inside it, never its own files replaced)
        var empty = !Directory.Exists(path) || !Directory.EnumerateFileSystemEntries(path).Any();
        var target = empty ? path : Path.Combine(path, "Triforce saves");
        if (!empty && Directory.Exists(target) && Directory.EnumerateFileSystemEntries(target).Any() && !Saves.IsSaveSet(target))
        {
            Toast("That folder already has a \"Triforce saves\" folder with other files in it. Choose an empty folder.", "error");
            return;
        }
        SavesAsk(false, Saves.Describe(empty ? "the folder " + Path.GetFileName(path.TrimEnd('\\', '/')) : "a new folder, Triforce saves", target));
    }

    /// <summary>Step two: a plain question that names what gets replaced, and that it is backed up first.</summary>
    void SavesAsk(bool into, SaveSet set)
    {
        var game = GameDir;
        if (game == null) return;
        var key = (into ? "in:" : "out:") + Guid.NewGuid().ToString("N");
        savesPending[key] = set;
        var ours = Saves.Describe("Triforce", new Saves(ctx).TriforceNewest(game));
        string html;
        if (into)
            html = $"<p>The saves from <b>{H(set.Name)}</b> ({H(SetLine(set))}) become Triforce's.</p>" +
                   (ours.Empty ? "<p>Triforce has no saves yet, so nothing is replaced.</p>"
                               : $"<p>Triforce's own saves ({H(SetLine(ours))}) are replaced. They are backed up first, so you can go back to them.</p>");
        else
            html = $"<p>Triforce's saves ({H(SetLine(ours))}) are copied to <b>{H(set.Name)}</b>.</p>" +
                   (set.Empty ? "<p>Nothing is there yet, so nothing is replaced.</p>"
                              : $"<p>What is there now ({H(SetLine(set))}) is replaced. It is backed up first, so you can go back to it.</p>") +
                   $"<p style=\"color:var(--text3);font-size:12.5px;overflow-wrap:anywhere\">{H(set.Folder)}</p>";
        Ask(into ? "Bring saves in" : "Copy saves out", html, "saves.confirm",
            new[] { ("cancel", "Cancel", false), ("go", into ? "Bring saves in" : "Copy saves out", true) }, key);
    }

    async Task SavesConfirm(string? id, string? key)
    {
        if (key == null || !savesPending.TryRemove(key, out var set) || id != "go") return;
        var game = GameDir;
        if (game == null) return;
        var into = key.StartsWith("in:", StringComparison.Ordinal);
        var saves = new Saves(ctx);
        string? backup;
        try
        {
            backup = into ? await saves.ImportAsync(set, game) : await saves.ExportAsync(game, set);
        }
        catch (Exception e) when (e is SavesException || e is InvalidOperationException || e is IOException || e is UnauthorizedAccessException)
        {
            Log.Write("saves: " + e.Message);
            Toast(H(e.Message), "error");
            return;
        }
        var kept = backup != null ? " The ones they replaced are backed up in save-backups." : "";
        Toast((into ? "Your saves are in." : "Your saves are copied to " + H(set.Name) + ".") + kept, "success");
        SendSaves();
        if (into) RefreshHome();
    }

    // ---------------- folders and links (shared by the three pages) ----------------

    /// <summary>A folder in the system's file window. A test run opens no windows: it says so instead.</summary>
    void OpenPlace(string folder)
    {
        if (!Directory.Exists(folder)) { Toast("That folder is not there: " + H(folder), "error"); return; }
        if (ctx.TestRoot != null) { Toast("Test run: no window opens. " + H(folder)); Log.Write("open folder (test run, not opened): " + folder); return; }
        ctx.Platform.OpenFolder(folder);
    }

    /// <summary>A web page in the player's browser (https only). A test run opens nothing: it says so instead.</summary>
    void OpenLink(string url)
    {
        if (!url.StartsWith("https://", StringComparison.OrdinalIgnoreCase)) return;
        if (ctx.TestRoot != null) { Toast("Test run: no browser opens. " + H(url)); Log.Write("open link (test run, not opened): " + url); return; }
        try
        {
            if (OperatingSystem.IsWindows()) System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(url) { UseShellExecute = true })?.Dispose();
            else
            {
                var si = new System.Diagnostics.ProcessStartInfo("xdg-open") { UseShellExecute = false };
                si.ArgumentList.Add(url);
                System.Diagnostics.Process.Start(si)?.Dispose();
            }
        }
        catch (Exception e) { Log.Write("open " + url + ": " + e.Message); Toast("The page could not be opened: " + H(url), "error"); }
    }
}
