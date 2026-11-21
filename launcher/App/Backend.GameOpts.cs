using System.Text.Json.Nodes;
using Triforce.Logic;

namespace Triforce.Launcher;

/// <summary>Settings > Display / Graphics / Audio / Controls / Game: the game's own Options menu (Triforce.Logic
/// GameOptionsSchema, its rows as the menu has them), read from and written to the game folder's triforce_options.ini the way
/// the game merges it. Refused while the game runs (it keeps its settings in memory and writes them back).</summary>
public sealed partial class Backend
{
    static readonly (GameOptionTab tab, string id, string name)[] GameTabs =
    {
        (GameOptionTab.Display, "display", "Display"), (GameOptionTab.Graphics, "graphics", "Graphics"),
        (GameOptionTab.Audio, "audio", "Audio"), (GameOptionTab.Controls, "controls", "Controls"), (GameOptionTab.System, "system", "Game"),
    };
    bool gpusHooked;

    void RegisterGameOpts()
    {
        On("gopt.choose", m =>
        {
            var key = (string?)m["key"] ?? "";
            if (!CanChangeGame(out var f) || GameOptionsSchema.ByKey(key) is not { } row) return;
            var now = f!.Get(key);
            var text = H(row.Help ?? "") + (row.Restart ? "<br><span style=\"color:var(--text3)\">Takes effect the next time the game starts.</span>" : "");
            if (key == Benchmark.KPreset)
                text += "<br>" + string.Join("<br>", GameOptionsSchema.PresetSummaries.Select((s, i) => i == 0 ? "" : "<b>" + H(f.ChoicesOf(key).ElementAtOrDefault(i)?.Label) + "</b> " + H(s)).Where(x => x.Length > 0));
            var choices = f.ChoicesOf(key).AsEnumerable();
            if (key == Benchmark.KUpscaler)
            {
                // (as the game's menu offers them: DLSS and DLAA with an NVIDIA card and DLSS in the build; FSR 4 on AMD cards)
                var dlss = Gpus.HasNvidia() && new Game(ctx).HasDlss(GameDir!);
                var amd = Gpus.List().Any(x => x.Vendor == GpuVendor.Amd);
                choices = choices.Where(c => c.Value == now || (c.Value is not (1 or 2) || dlss) && (c.Value is not (4 or 5) || amd));
            }
            Choice(row.Name, text, "gopt.set", choices.Select(c => (c.Value.ToString(), c.Label, (string?)null, c.Value == now)), "Choose", key);
        });
        On("gopt.set", m =>
        {
            var key = (string?)m["ctx"] ?? (string?)m["key"] ?? "";
            if (!int.TryParse((string?)m["id"] ?? m["value"]?.ToString(), out var v)) return;
            SetGameOption(key, v);
        });
        On("gopt.value", m =>
        {
            var key = (string?)m["key"] ?? "";
            if (m["value"] is JsonNode n && int.TryParse(n.ToString(), out var v)) SetGameOption(key, v);
        });
    }

    void SetGameOption(string key, int v)
    {
        if (!CanChangeGame(out var f) || GameOptionsSchema.ByKey(key) == null) { RefreshSettingsState(); return; }
        f!.Set(key, v);
        f.Save();
        Log.Write("game option " + key + "=" + f.Get(key) + (f.Changes.Count > 1 ? " (with " + string.Join(", ", f.Changes.Where(c => c.Key != key).Select(c => c.Key + "=" + c.Value)) + ")" : ""));
        RefreshSettingsState();
    }

    /// <summary>The game's options may be written now: a built game, not running, and (a test run) the folder allowed.</summary>
    bool CanChangeGame(out GameOptionsFile? file)
    {
        file = null;
        var g = GameDir;
        if (g == null) return false;
        if (new Game(ctx).IsRunning(g)) { Toast("Close the game to change these: it keeps its own settings while it runs.", "error"); return false; }
        var path = Path.Combine(g, GameOptionsSchema.FileName);
        if (!ctx.AllowedForGame(path)) { Toast(H(ctx.TestOnly), "error"); return false; }
        file = GameOptionsFile.Load(g, MainScreen());
        return true;
    }

    JsonObject? GameOptsState()
    {
        var g = GameDir;
        if (g == null) return null;
        if (!gpusHooked) { GameOptionsSchema.UseGameGpus(() => GameDir); gpusHooked = true; }   // ((Triforce) 11-19: the game's own order)
        GameOptionsFile f;
        try { f = GameOptionsFile.Load(g, MainScreen()); }
        catch (Exception e) { Log.Write("game options: " + e.Message); return null; }
        var running = new Game(ctx).IsRunning(g);
        var tabs = new JsonArray();
        var data = new JsonObject();
        foreach (var (tab, id, name) in GameTabs)
        {
            tabs.Add(new JsonObject { ["id"] = id, ["name"] = name });
            var sections = new JsonArray();
            JsonObject? cur = null;
            foreach (var r in GameOptionsSchema.RowsOf(tab))
            {
                var h = r.Section ?? name;
                if (cur == null || (string?)cur["h"] != h) { cur = new JsonObject { ["h"] = h, ["rows"] = new JsonArray() }; sections.Add(cur); }
                var v = f.Get(r.Key);
                ((JsonArray)cur["rows"]!).Add(new JsonObject
                {
                    ["key"] = r.Key, ["label"] = r.Name, ["value"] = f.Label(r.Key), ["v"] = v,
                    ["kind"] = r.Kind == GameOptionKind.Slider ? "slider" : "choice",
                    ["lo"] = r.Lo, ["hi"] = r.Hi, ["step"] = r.Step, ["restart"] = r.Restart, ["help"] = r.Help,
                });
            }
            data[id] = sections;
        }
        return new JsonObject
        {
            ["tabs"] = tabs, ["data"] = data, ["locked"] = running,
            ["note"] = running ? "Close the game to change these: it keeps its own settings while it runs." : "The same settings as the game's own menu. They take effect the next time you play.",
        };
    }

    static (int W, int H)? MainScreen()
    {
        if (!OperatingSystem.IsWindows()) return null;
        try { return (GetSystemMetrics(0), GetSystemMetrics(1)); } catch { return null; }
    }
    [System.Runtime.InteropServices.DllImport("user32")] static extern int GetSystemMetrics(int i);
}
