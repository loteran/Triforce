// Installing what GameBanana offers (vk_mods.cpp run_install / run_kit), with the launcher's own checked unpacking (ModManager:
// names, links, sizes, room) where the game used tar.exe as is.
//   a mod   downloaded into mods\.triforce-download, unpacked there, each folder holding romfs (or exefs) found (five levels down;
//           a folder of the game's own RomFS folders with no romfs above them is wrapped in one), then moved into mods\NN-<name>
//           with triforce_mod.json beside it. A reinstall or an update replaces the folders this mod had and keeps the first one's
//           place in the order and its on/off; several roots are the mod's options: the first on, the others off (_).
//   a kit   (Kits: an allow-list, never what a download holds) unpacked into kits\<folder>, its start file found (three levels
//           down), its files moved over what is there (the kit's own settings stay), triforce_kit.json written, then its start file
//           run in a console of its own. Windows only, as in the game.
using System.Diagnostics;
using System.Text.Json;

namespace Triforce.Logic;

/// <summary>A program of its own posted in the section, not a romfs mod.</summary>
public sealed record Kit(bool Wip, int Id, string Folder, string Start);

public static class Kits
{
    /// <summary>((Triforce) 11-18, "make sure the multiplayer mod is included": daurosk's Squadala!, online co-op,
    /// github.com/daurosk/oot-remake-multiplayer.) Only the ones named here: a download is never started because of what it holds.</summary>
    public static readonly IReadOnlyList<Kit> All = new[] { new Kit(true, 105323, "Squadala", "Start Squadala.bat") };

    public static Kit? Find(bool wip, int id) => All.FirstOrDefault(k => k.Wip == wip && k.Id == id);
}

/// <summary>A mod or kit that came from GameBanana, as its json says (id, WiP or Mod, the file's date).</summary>
public sealed record InstalledMod(int Id, bool Wip, long FileDate, string Folder);

public sealed class GameBananaInstaller
{
    readonly TriforceContext ctx;
    readonly ModManager mods;
    public GameBananaInstaller(TriforceContext ctx, ModManager? mods = null)
    {
        this.ctx = ctx;
        this.mods = mods ?? new ModManager(ctx);
    }

    static readonly string[] RomfsDirs = { "Model", "UI", "Tex", "Pack", "Sound", "Event", "Banc", "Effect", "Mals", "Font2", "Logic", "Phive", "Scene", "System", "GameData", "Voice", "Movie", "AS", "AI", "Game", "Env" };

    /// <summary>What came from GameBanana into this game folder: mods (triforce_mod.json) and kits (triforce_kit.json).</summary>
    public IReadOnlyList<InstalledMod> Installed(string game)
    {
        var o = new List<InstalledMod>();
        var root = Mods.FolderOf(game);
        if (Directory.Exists(root))
            foreach (var d in Directory.GetDirectories(root))
            {
                if (Path.GetFileName(d).StartsWith('.')) continue;
                if (ModManager.ReadInfo(d) is { GameBananaId: > 0 } i) o.Add(new InstalledMod((int)i.GameBananaId, i.Wip, i.FileDate, d));
            }
        foreach (var k in Kits.All)
        {
            var dir = Path.Combine(game, "kits", k.Folder);
            if (ReadJson(Path.Combine(dir, "triforce_kit.json")) is { } j && j.Id == k.Id) o.Add(new InstalledMod(k.Id, k.Wip, j.Date, dir));
        }
        return o;
    }

    /// <summary>Installed, and is there a newer file (an "Update" mark): newest file date &gt; the one installed.</summary>
    public (bool Installed, bool Update) State(string game, GbMod m, GbDetails? d)
    {
        var hit = Installed(game).FirstOrDefault(i => i.Id == m.Id && i.Wip == m.Wip);
        if (hit == null) return (false, false);
        var newest = d?.Files.Count > 0 ? d.Files.Max(f => f.Added) : 0;
        return (true, newest > hit.FileDate && hit.FileDate > 0);
    }

    static (long Id, long Date)? ReadJson(string file)
    {
        try
        {
            if (!File.Exists(file)) return null;
            using var doc = JsonDocument.Parse(File.ReadAllText(file));
            var r = doc.RootElement;
            long N(string n) => r.TryGetProperty(n, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt64(out var x) ? x : 0;
            return (N("gamebanana_id"), N("file_date"));
        }
        catch (Exception) { return null; }
    }

    /// <summary>A folder name: no characters Windows refuses, never "off" by its name, at most 60 characters (vk_mods.cpp safe_name).</summary>
    public static string SafeName(string name)
    {
        var s = new string(name.Where(c => c >= 32 && "<>:\"/\\|?*".IndexOf(c) < 0).ToArray());
        s = s.TrimEnd('.', ' ').TrimStart('_', '-', '.', ' ');
        if (s.Length > 60) s = s.Substring(0, 60);
        s = s.TrimEnd('.', ' ');
        return s.Length == 0 ? "mod" : s;
    }

    static string InfoJson(string name, GbMod m, GbFile f, bool kit) => JsonSerializer.Serialize(new Dictionary<string, object?>
    {
        ["name"] = name, ["author"] = m.Author, ["page"] = m.Page, ["gamebanana_id"] = m.Id,
        ["gamebanana_model"] = kit || m.Wip ? (m.Wip ? "Wip" : "Mod") : null,
        ["file"] = f.Name, ["file_date"] = f.Added, ["installed_by"] = "Triforce's launcher",
    }.Where(kv => kv.Value != null).ToDictionary(kv => kv.Key, kv => kv.Value), new JsonSerializerOptions { WriteIndented = true }) + "\n";

    /// <summary>Downloads and installs one of a mod's files; a kit goes to kits and is started. Returns the sentence the App shows.
    /// Refused while the game runs.</summary>
    public async Task<string> InstallAsync(HttpClient http, GbMod m, GbFile f, string game, Action<DownloadProgress>? progress, CancellationToken token)
    {
        if (Kits.Find(m.Wip, m.Id) is { } kit) return await InstallKitAsync(http, m, f, game, kit, progress, token).ConfigureAwait(false);
        if (mods.GameRuns(game)) throw new ModsException("Close the game first. Mods are added, switched, ordered and removed only while it is not running.");
        var root = Mods.FolderOf(game);
        var stage = Path.Combine(root, ".triforce-download");
        ctx.Require(stage);
        var unpacked = await DownloadUnpackAsync(http, m, f, root, stage, progress, token).ConfigureAwait(false);
        try
        {
            var (roots, bare) = FindRoots(unpacked);
            if (roots.Count == 0 && bare.Count > 0)   // (packed without a romfs folder: one made around the game's folders)
            {
                var made = Path.Combine(stage, "wrapped");
                Directory.CreateDirectory(made);
                Directory.Move(bare[0], Path.Combine(made, "romfs"));
                roots.Add(made);
            }
            if (roots.Count == 0)
                throw new ModsException("This download has no romfs folder in it, so it is not a mod Triforce can load (it may need its own installer, or be for another version of the game).");
            // (a reinstall or an update: the folders this mod had go, and the new one takes the first one's place and its on or off)
            var number = 0;
            var wasOff = false;
            var old = new List<string>();
            foreach (var d in Directory.GetDirectories(root).OrderBy(x => x, StringComparer.Ordinal))
            {
                var name = Path.GetFileName(d);
                if (name.StartsWith('.')) continue;
                if (ModManager.ReadInfo(d) is not { } info || info.GameBananaId != m.Id) continue;
                if (old.Count == 0)
                {
                    var (off, num, _) = Mods.Parse(name);
                    wasOff = off;
                    number = num ?? 0;
                }
                old.Add(d);
            }
            var aside = new List<(string Was, string Gone)>();
            for (var k = 0; k < old.Count; k++)   // (out of the way first: removed once the new one is in, put back if it fails)
            {
                var gone = Path.Combine(stage, "old-" + k);
                try { Directory.Move(old[k], gone); aside.Add((old[k], gone)); } catch (Exception e) { ctx.Log.Write("mods: the old " + old[k] + " could not be moved aside: " + e.Message); }
            }
            var label0 = SafeName(m.Name);
            if (number == 0) number = NextNumber(root);
            var placed = new List<string>();
            string? err = null;
            for (var i = 0; i < roots.Count; i++)
            {
                var label = label0;
                if (roots.Count > 1 && !TriforceContext.SamePath(roots[i], unpacked)) label += " - " + SafeName(Path.GetFileName(roots[i]));
                var prefix = (i > 0 || wasOff ? "_" : "") + (number++).ToString("00") + "-";
                var target = Path.Combine(root, prefix + label);
                for (var k = 2; Directory.Exists(target); k++) target = Path.Combine(root, prefix + label + " (" + k + ")");
                try { ctx.Require(target); Directory.Move(roots[i], target); }
                catch (Exception e) { err = "could not move it into " + target + " (" + e.Message + ")"; break; }
                File.WriteAllText(Path.Combine(target, "triforce_mod.json"), InfoJson(roots.Count > 1 ? label : m.Name, m, f, false));
                placed.Add(target);
            }
            if (placed.Count == 0)
            {
                foreach (var (was, gone) in aside) try { Directory.Move(gone, was); } catch (Exception) { }
                throw new ModsException("It could not be installed: " + err + "." + (aside.Count == 0 ? "" : " The one installed before is kept."));
            }
            if (placed.Select(p => mods.List(root).FirstOrDefault(x => TriforceContext.SamePath(x.Path, p))).Any(x => x is { Built: true }) && mods.BuiltMark(game) == null)
                mods.MarkChanged(game);
            ctx.Log.Write("mods: installed " + m.Name + " (" + m.Id + ") into " + placed.Count + " folder(s), the first " + placed[0]);
            var msg = "Installed into mods" + Path.DirectorySeparatorChar + Path.GetFileName(placed[0]) + ".";
            if (placed.Count > 1) msg += " It came with " + placed.Count + " options: the first is on, the others are off. Choose on the Mods page.";
            return msg;
        }
        finally { Partial.Remove(stage); }
    }

    /// <summary>The file downloaded into stage (made anew) and unpacked into stage\unpacked with the checked unpacking.</summary>
    async Task<string> DownloadUnpackAsync(HttpClient http, GbMod m, GbFile f, string within, string stage, Action<DownloadProgress>? progress, CancellationToken token)
    {
        Partial.Remove(stage);
        Directory.CreateDirectory(stage);
        try { File.SetAttributes(stage, File.GetAttributes(stage) | FileAttributes.Hidden); } catch (Exception) { }
        var name = SafeName(f.Name.Length == 0 ? "mod.zip" : f.Name);
        var archive = Path.Combine(stage, name);
        ctx.Log.Write("mods: installing " + m.Name + " (" + m.Id + "), file " + f.Name + " (" + f.Bytes + " bytes) from " + f.Url);
        try
        {
            // (the size the server names: GameBanana's listed size is not trusted to the byte)
            await new Downloader(ctx).ToFileAsync(http, f.Url, archive, 0, progress, token).ConfigureAwait(false);
            var unpacked = Path.Combine(stage, "unpacked");
            await Task.Run(() => mods.Unpack(within, archive, unpacked, null), token).ConfigureAwait(false);
            return unpacked;
        }
        catch (Exception)
        {
            Partial.Remove(stage);
            throw;
        }
    }

    /// <summary>The folders holding romfs (or exefs) under dir, five levels down; a folder of the game's RomFS folders with no romfs
    /// above them counts as a romfs itself (bare).</summary>
    static (List<string> Roots, List<string> Bare) FindRoots(string dir)
    {
        var roots = new List<string>();
        var bare = new List<string>();
        void Look(string d, int depth)
        {
            if (Directory.Exists(Path.Combine(d, "romfs")) || Directory.Exists(Path.Combine(d, "exefs"))) { roots.Add(d); return; }
            var subs = Directory.GetDirectories(d).OrderBy(x => x, StringComparer.Ordinal).ToList();
            if (subs.Any(s => RomfsDirs.Any(r => string.Equals(Path.GetFileName(s), r, StringComparison.OrdinalIgnoreCase)))) { bare.Add(d); return; }
            if (depth >= 5) return;
            foreach (var s in subs) Look(s, depth + 1);
        }
        Look(dir, 0);
        return (roots, bare);
    }

    /// <summary>After the highest "NN-" there: a new mod wins over those installed before.</summary>
    static int NextNumber(string root)
    {
        var hi = 0;
        foreach (var d in Directory.GetDirectories(root))
            if (Mods.Parse(Path.GetFileName(d)).Number is { } n) hi = Math.Max(hi, n);
        return hi + 1;
    }

    async Task<string> InstallKitAsync(HttpClient http, GbMod m, GbFile f, string game, Kit k, Action<DownloadProgress>? progress, CancellationToken token)
    {
        if (!OperatingSystem.IsWindows()) throw new ModsException(m.Name + " is not available on Linux from the launcher yet: download it from its page and follow its own steps.");
        var kits = Path.Combine(game, "kits");
        var dir = Path.Combine(kits, k.Folder);
        var stage = Path.Combine(kits, ".triforce-download");
        ctx.Require(stage);
        // (this file installed already, and its start file there: started again, nothing downloaded)
        var have = ReadJson(Path.Combine(dir, "triforce_kit.json")) is { } j && j.Id == k.Id && j.Date >= f.Added && File.Exists(Path.Combine(dir, k.Start));
        if (!have)
        {
            ctx.Log.Write("mods: " + m.Name + " is a kit: into " + dir);
            var unpacked = await DownloadUnpackAsync(http, m, f, kits, stage, progress, token).ConfigureAwait(false);
            try
            {
                var top = FindKitTop(unpacked, k.Start, 0)
                          ?? throw new ModsException("This download has no \"" + k.Start + "\" in it: GameBanana's file may have changed. Nothing was installed.");
                try { MoveOver(top, dir); }
                catch (Exception e) { throw new ModsException("It could not be put into kits\\" + k.Folder + ": " + e.Message + ". If " + m.Name + " is running, close it and try again."); }
                File.WriteAllText(Path.Combine(dir, "triforce_kit.json"), InfoJson(m.Name, m, f, true));
            }
            finally { Partial.Remove(stage); }
        }
        if (!ctx.Allowed(dir)) throw new ModsException(ctx.TestOnly);
        try
        {
            // (the kit's start file run in a console of its own, from its folder: cmd /c, a .bat or any start file Windows runs)
            var cmd = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "cmd.exe");
            Process.Start(new ProcessStartInfo(cmd, "/c \"\"" + Path.Combine(dir, k.Start) + "\"\"") { UseShellExecute = false, CreateNoWindow = false, WorkingDirectory = dir })?.Dispose();
        }
        catch (Exception e)
        {
            throw new ModsException(k.Start + " could not be started (" + e.Message + "). It is in kits\\" + k.Folder + " beside the game: start it from there.");
        }
        ctx.Log.Write("mods: kit " + m.Name + " " + (have ? "installed already" : "installed") + ", started: " + Path.Combine(dir, k.Start));
        return (have ? "Started" : "Installed into kits\\" + k.Folder + " and started") + " in a window of its own. Follow its steps there.";
    }

    /// <summary>The folder under dir (dir itself, or 3 levels down at most) whose files hold the kit's start file.</summary>
    static string? FindKitTop(string dir, string start, int depth)
    {
        if (Directory.GetFiles(dir).Any(f => string.Equals(Path.GetFileName(f), start, StringComparison.OrdinalIgnoreCase))) return dir;
        if (depth >= 3) return null;
        foreach (var d in Directory.GetDirectories(dir).OrderBy(x => x, StringComparer.Ordinal))
            if (FindKitTop(d, start, depth + 1) is { } t) return t;
        return null;
    }

    /// <summary>from's files moved over into's (folders made, files there replaced; what into has more stays: the kit's own settings).</summary>
    static void MoveOver(string from, string into)
    {
        Directory.CreateDirectory(into);
        foreach (var f in Directory.GetFiles(from))
        {
            var to = Path.Combine(into, Path.GetFileName(f));
            if (File.Exists(to)) File.SetAttributes(to, FileAttributes.Normal);
            File.Move(f, to, true);
        }
        foreach (var d in Directory.GetDirectories(from)) MoveOver(d, Path.Combine(into, Path.GetFileName(d)));
    }
}
