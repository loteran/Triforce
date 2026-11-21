using System.Text.Json.Nodes;
using Triforce.Launcher.Host;
using Triforce.Logic;

namespace Triforce.Launcher;

/// <summary>The launcher's side of the page: takes the page's messages (JSON with a "t" type; commands are
/// {t:'cmd', c:name, ...}) and answers with state. The logic lives in Triforce.Logic (install, release, build, saves, mods)
/// and Triforce.Core (the game's files). Each area is its own partial file and registers its commands:
///   Backend.Core.cs      first run, build, home, update, Play           (RegisterCore)
///   Backend.Saves.cs     the Saves page                                  (RegisterSaves)
///   Backend.Mods.cs      the Mods page                                   (RegisterMods)
///   Backend.Settings.cs  the Settings page, sound, uninstall             (RegisterSettings)</summary>
public sealed partial class Backend
{
    readonly IHost host;
    readonly Content content;
    readonly string dataDir;
    readonly TriforceContext ctx;
    readonly StartupResult start;
    readonly Dictionary<string, Action<JsonNode, string[]>> cmds = new();

    public Backend(IHost host, Content content, TriforceContext ctx, StartupResult start, string dataDir)
    {
        this.host = host; this.content = content; this.ctx = ctx; this.start = start; this.dataDir = dataDir;
        content.Media = MediaFor;
        content.SavePicture = SavePicture;
        RegisterCore(); RegisterSaves(); RegisterMods(); RegisterSettings(); RegisterBench(); RegisterGameOpts();
    }

    partial void RegisterSaves();
    partial void RegisterMods();
    partial void RegisterSettings();

    /// <summary>A command from the page: {t:'cmd', c:name, ...}. Handlers run on a worker thread; anything slow is fine.</summary>
    void On(string name, Action<JsonNode, string[]> handler) => cmds[name] = handler;
    void On(string name, Action<JsonNode> handler) => cmds[name] = (m, _) => handler(m);
    void OnAsync(string name, Func<JsonNode, Task> handler) => cmds[name] = (m, f) => { _ = Guarded(name, () => handler(m)); };

    async Task Guarded(string what, Func<Task> f)
    {
        try { await f(); }
        catch (Exception e)
        {
            Log.Write(what + ": " + e);
            Toast("Something went wrong: " + H(e.Message), "error");
        }
    }

    public void OnMessage(string json, string[] files)
    {
        JsonNode? m;
        try { m = JsonNode.Parse(json); } catch { Log.Write("bad message " + json); return; }
        if (m == null) return;
        var t = (string?)m["t"];
        try
        {
            switch (t)
            {
                case "ready": OnReady(); break;
                case "win":
                    switch ((string?)m["a"])
                    {
                        case "min": host.Minimize(); break;
                        case "max": host.ToggleMaximize(); break;
                        case "close": RequestClose(); break;
                    }
                    break;
                case "log": Log.Write("page: " + (string?)m["s"]); break;
                case "pick": OnPick((string?)m["kind"] == "folder"); break;
                case "drop": if (files.Length > 0) _ = Intake(files[0]); break;
                case "cmd":
                    var c = (string?)m["c"] ?? "";
                    // (a color is taken here, in the order the page sent it, so a 'settings.list' after it sends it back)
                    if (c == "color") ColorArrived(m);
                    if (cmds.TryGetValue(c, out var h))
                        Task.Run(() => { try { h(m, files); } catch (Exception e) { Log.Write("cmd " + c + ": " + e); Toast("Something went wrong: " + H(e.Message), "error"); } });
                    else Log.Write("unhandled cmd " + c);
                    break;
            }
        }
        catch (Exception e)
        {
            Log.Write("message " + t + ": " + e);
        }
    }

    // ---------------- talking to the page ----------------
    public void Send(JsonNode n) => host.Post(n.ToJsonString());
    /// <summary>Parts of the page's state, e.g. SendState(new() { ["saves"] = ... }).</summary>
    public void SendState(JsonObject parts) { parts["t"] = "state"; Send(parts); }
    public void Go(string screen) => Send(new JsonObject { ["t"] = "go", ["s"] = screen });
    public void Sfx(string id) => Send(new JsonObject { ["t"] = "sfx", ["id"] = id });
    /// <summary>A short line at the top (HTML: keep it to b/em/span; encode anything from outside with H()).</summary>
    public void Toast(string html, string? sfx = null)
    {
        var o = new JsonObject { ["t"] = "toast", ["html"] = html };
        if (sfx != null) o["sfx"] = sfx;
        Send(o);
    }
    public void Copy(string text) => Send(new JsonObject { ["t"] = "copy", ["text"] = text });

    /// <summary>A list to choose one from. The choice comes back as the command <paramref name="reply"/> with {id, ctx}.</summary>
    public void Choice(string title, string? textHtml, string reply, IEnumerable<(string id, string name, string? sub, bool sel)> options, string ok = "Choose", string? context = null)
    {
        var arr = new JsonArray();
        foreach (var (id, name, sub, sel) in options) arr.Add(new JsonObject { ["id"] = id, ["name"] = name, ["sub"] = sub, ["sel"] = sel });
        Send(new JsonObject { ["t"] = "choice", ["title"] = title, ["text"] = textHtml, ["reply"] = reply, ["options"] = arr, ["ok"] = ok, ["ctx"] = context });
    }

    /// <summary>A question with buttons. The pressed button's id comes back as the command <paramref name="reply"/> with {id, ctx}.
    /// One button at most is the main (gold) one.</summary>
    public void Ask(string title, string html, string reply, IEnumerable<(string id, string label, bool main)> buttons, string? context = null)
    {
        var arr = new JsonArray();
        foreach (var (id, label, main) in buttons) arr.Add(new JsonObject { ["id"] = id, ["label"] = label, ["main"] = main });
        Send(new JsonObject { ["t"] = "ask", ["title"] = title, ["html"] = html, ["reply"] = reply, ["buttons"] = arr, ["ctx"] = context });
    }

    public static string H(string? s) => System.Net.WebUtility.HtmlEncode(s ?? "");

    /// <summary>The built game folder (null before the first build).</summary>
    public string? GameDir => new Game(ctx).Current;

    /// <summary>A file / folder picker on the window's thread (it must be), then the answer on a worker.</summary>
    public void PickThen(bool folder, string title, Action<string> then, string filterName = "All files", string pattern = "*.*")
    {
        host.Invoke(() =>
        {
            var path = folder ? host.PickFolder(title) : host.PickFile(title, filterName, pattern);
            if (path != null) Task.Run(() => { try { then(path); } catch (Exception e) { Log.Write("pick: " + e); Toast("Something went wrong: " + H(e.Message), "error"); } });
        });
    }

    void OnPick(bool folder) =>
        PickThen(folder, folder ? "Choose the folder with romfs and exefs in it" : "Choose your game", p => _ = Intake(p), "Game file (*.nup)", "*.nup");

    public void Shutdown()
    {
        try { StopEverything(); } catch { }
        try { start.Lock?.Dispose(); } catch { }
    }
}
