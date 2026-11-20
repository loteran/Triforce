// (Triforce) The Mods page, reached from "Your game" on the welcome-back page once a game is built. It shows the game folder's
// mods folder as the game reads it and changes it only by what a player could do by hand (Mods.cs: a "-" in front of a
// folder's name, a number in front, a folder moved in or out).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;

namespace TriforceSetup;

sealed partial class Shell
{
    Grid ModsPage = null!;
    ListBox ModList = null!;
    TextBlock ModsInfo = null!;
    Button ModsButton = null!, ModsAdd = null!, ModsOpen = null!, ModsRescan = null!, ModsConflicts = null!, ModsBack = null!, ModsUp = null!,
        ModsDown = null!, ModsRemove = null!, ModsToggle = null!, ModsBuild = null!;
    List<Mod> mods = new();
    List<Conflict> conflicts = new();
    bool modsBusy;
    string? modsSaid;   // (what the last change ended with, shown until another mod is picked)

    void InitMods()
    {
        ModsPage = Part<Grid>("ModsPage");
        ModList = Part<ListBox>("ModList");
        ModsInfo = Part<TextBlock>("ModsInfo");
        ModsButton = Part<Button>("ModsButton");
        ModsAdd = Part<Button>("ModsAdd"); ModsOpen = Part<Button>("ModsOpen"); ModsRescan = Part<Button>("ModsRescan"); ModsConflicts = Part<Button>("ModsConflicts");
        ModsBack = Part<Button>("ModsBack"); ModsUp = Part<Button>("ModsUp"); ModsDown = Part<Button>("ModsDown"); ModsRemove = Part<Button>("ModsRemove");
        ModsToggle = Part<Button>("ModsToggle"); ModsBuild = Part<Button>("ModsBuild");

        // (no folder is renamed while the game runs: Mods.cs asks this before every change)
        Mods.GameRuns = GameRuns;

        ModsButton.Click += (_, _) => ShowMods();
        ModsBack.Click += (_, _) => ShowHome(homeOffline);
        ModsRescan.Click += (_, _) => _ = LoadMods(PickedMod?.Path, null);
        ModsOpen.Click += (_, _) => OpenModsFolder();
        ModsAdd.Click += (_, _) => PickArchive();
        ModsConflicts.Click += (_, _) => CopyConflicts();
        ModsToggle.Click += (_, _) => { if (PickedMod is { } m) ChangeMods(m, folder => Mods.SetOn(folder, m, !m.On), m.Name + " is " + (m.On ? "off" : "on") + " from the game's next start."); };
        ModsUp.Click += (_, _) => { if (PickedMod is { } m) ChangeMods(m, folder => Mods.Move(folder, m, false) ? null : m.Path, null); };
        ModsDown.Click += (_, _) => { if (PickedMod is { } m) ChangeMods(m, folder => Mods.Move(folder, m, true) ? null : m.Path, null); };
        ModsRemove.Click += (_, _) =>
        {
            if (PickedMod is not { } m) return;
            Ask("Remove " + m.Name + "? Its folder moves into mods\\" + Mods.Removed + ", where the game does not look. Nothing is deleted.", "Remove", T(S.Back),
                () => ChangeMods(m, folder => { Mods.Remove(folder, m); return null; }, m.Name + " was removed. Its folder is kept in mods\\" + Mods.Removed + "."));
        };
        ModsBuild.Click += (_, _) => BuildForMods();
        ModList.SelectionChanged += (_, _) => { if (!modsBusy) { modsSaid = null; ModsRefresh(); } };
        Window.Drop += (_, e) =>
        {
            if (ModsPage.Visibility == Visibility.Visible && e.Data.GetData(DataFormats.FileDrop) is string[] { Length: > 0 } paths) AddMod(paths[0]);
        };
    }

    /// Does a program of this name run from inside a folder? One whose own folder cannot be asked counts when unknown says so
    static bool Running(string name, string folder, bool unknown)
    {
        foreach (var p in Process.GetProcessesByName(name))
            using (p)
            {
                string? exe = null;
                try { exe = p.MainModule?.FileName; } catch (Exception) { }
                if (exe == null ? unknown : Mods.Inside(folder, exe)) return true;
            }
        return false;
    }

    /// Does the game of this game folder run? It read its mods folder when it started, and a folder renamed under it is a
    /// file it can no longer open. (Another game folder's game is another game. In a test run only a game inside the test
    /// folder counts, as for the saves: Saves.cs RunningGame.)
    static bool GameRuns(string folder) => new[] { "Triforce", "hookshot" }.Any(name => Running(name, folder, Install.TestRoot == null));

    // ---- the page ----

    static string? ModsFolder() => GameFolder() is { } game ? Mods.FolderOf(game) : null;
    Mod? PickedMod => (ModList.SelectedItem as FrameworkElement)?.Tag as Mod;

    void ShowMods()
    {
        Show(ModsPage);
        _ = LoadMods(null, null);
    }

    /// The mods folder read again, and shown: pick is the folder of the mod to have picked after it, said what to say under the list
    async Task LoadMods(string? pick, string? said, bool bad = false)
    {
        var folder = ModsFolder();
        List<Mod> found;
        try { found = folder != null ? await Task.Run(() => Mods.List(folder)) : new List<Mod>(); }
        catch (Exception e) { found = new List<Mod>(); said = "The mods folder could not be read: " + e.Message; bad = true; }
        ModsFill(found, pick, said, bad);
    }

    /// The page for a list of mods (also what --snapshot shows, with sample mods)
    void ModsFill(List<Mod> found, string? pick, string? said, bool bad = false)
    {
        mods = found;
        conflicts = Mods.Conflicts(mods);
        LoadSounds();   // (a mod may carry one of the two sounds Setup plays, or no longer: Shell.Sounds.cs)
        modsBusy = true;   // (the list being filled is no pick of the player's)
        ModList.ItemsSource = mods.Select(ModRow).ToList();
        var at = pick != null ? mods.FindIndex(m => string.Equals(m.Path, pick, StringComparison.OrdinalIgnoreCase)) : -1;
        ModList.SelectedIndex = at >= 0 ? at : mods.Count > 0 ? 0 : -1;
        if (ModList.SelectedItem != null) ModList.ScrollIntoView(ModList.SelectedItem);
        modsBusy = false;
        modsSaid = said;
        ModsRefresh(bad);
    }

    /// One mod: its name, on or off to the right, what it carries and whom it shares files with under it
    FrameworkElement ModRow(Mod m)
    {
        var g = new Grid { Tag = m };
        g.ColumnDefinitions.Add(new ColumnDefinition());
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.RowDefinitions.Add(new RowDefinition());
        g.RowDefinitions.Add(new RowDefinition());
        var name = new TextBlock
        {
            Text = m.Deep ? m.Name + "  (" + System.IO.Path.GetDirectoryName(m.Rel.Replace('/', '\\')) + ")" : m.Name, Foreground = Brush(m.On ? "TextHi" : "TextDim"), FontSize = 14,
            TextTrimming = TextTrimming.CharacterEllipsis, Margin = new Thickness(0, 0, 16, 0),
        };
        var state = new TextBlock { Text = m.On ? "On" : "Off", Foreground = Brush(m.On ? "Gold" : "TextFaint"), FontSize = 12, VerticalAlignment = VerticalAlignment.Center };
        var shared = Mods.Shared(m, mods, conflicts);
        var what = Mods.Carries(m);
        if (shared.Count == 1) what += "  ·  " + (shared[0].Wins ? "wins " : "loses ") + Files(shared[0].Files) + (shared[0].Wins ? " over " : " to ") + shared[0].Other.Name;
        else if (shared.Count > 1) what += "  ·  shares files with " + shared.Count + " mods";
        var carries = new TextBlock { Text = what, Foreground = Brush("TextFaint"), FontSize = 11.5, TextTrimming = TextTrimming.CharacterEllipsis, Margin = new Thickness(0, 3, 0, 0) };
        Grid.SetColumn(state, 1);
        Grid.SetRow(carries, 1);
        Grid.SetColumnSpan(carries, 2);
        g.Children.Add(name);
        g.Children.Add(state);
        g.Children.Add(carries);
        return g;
    }

    static string Files(int n) => n == 1 ? "1 file" : n.ToString("N0", System.Globalization.CultureInfo.InvariantCulture) + " files";

    /// What the page says about the mod that is picked: what it carries, when a change of it takes effect, and for each mod
    /// it shares files with, how many and who wins them
    string ModWords(Mod m)
    {
        var s = m.Name + " is " + (m.On ? "on" : m.OffAbove != null ? "off, as everything in the folder " + m.OffAbove + " is" : "off") + " and carries " + Mods.Carries(m) + ".";
        if (m.Built) s += " Code patches are built into the game: after a change of them, build the game again.";
        if (m.Cheats > 0) s += " Its cheats are read when the game starts; switch each one on in the game's Options, on the System tab.";
        if (m.Deep) s += " It keeps its place in the order together with its folder, " + m.Top + ".";
        foreach (var (other, files, wins) in Mods.Shared(m, mods, conflicts).Take(2))
        {
            var which = conflicts.Where(c => c.Mods.Contains(m) && c.Mods.Contains(other)).Select(c => c.File).ToList();
            s += " " + Files(files) + (files == 1 ? " is" : " are") + " also in " + other.Name + ", and " + (wins ? m.Name : other.Name) + " wins (" + which[0] + (which.Count > 1 ? ", ..." : "") + ").";
        }
        return s;
    }

    void ModsRefresh(bool bad = false)
    {
        var game = GameFolder();
        var m = PickedMod;
        var tops = Mods.Tops(mods);
        var at = m != null ? tops.FindIndex(t => string.Equals(t, m.Top, StringComparison.OrdinalIgnoreCase)) : -1;
        ModsToggle.IsEnabled = m != null && !modsBusy;
        ModsToggle.Content = m is { On: false } ? "Switch on" : "Switch off";
        ModsRemove.IsEnabled = m != null && !modsBusy;
        ModsUp.IsEnabled = at > 0 && !modsBusy;
        ModsDown.IsEnabled = at >= 0 && at < tops.Count - 1 && !modsBusy;
        ModsAdd.IsEnabled = ModsRescan.IsEnabled = !modsBusy;
        ModsConflicts.Content = "Copy conflicts";
        ModsConflicts.Visibility = conflicts.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        ModsInfo.Foreground = Brush(bad ? "Bad" : "Text");
        ModsInfo.Text = modsSaid ?? (game == null ? "Build your game first: mods live in its game folder."
            : m != null ? ModWords(m)
            : "No mods yet. Add a mod takes a .zip, .7z or .rar file; or put a mod's folder (the one that holds romfs) into " + Mods.FolderOf(game) + " yourself.");
        // (code patches and cheats are compiled in: said for as long as the mods that carry them are not the ones of the last build)
        var build = game != null && Mods.NeedsBuild(Mods.CodeState(mods), ModsBuilt(game));
        ModsBuild.Content = Mods.BuildAgain;
        ModsBuild.Visibility = build ? Visibility.Visible : Visibility.Collapsed;
    }

    /// One change of the mods, off the window's thread: change gets the mods folder and returns the folder of the mod to have
    /// picked afterwards (null: the mod where it is now, by its name). Then the folder is read again, and done is said
    async void ChangeMods(Mod? mod, Func<string, string?> change, string? done)
    {
        var folder = ModsFolder();
        if (folder == null || modsBusy) return;
        if (!Install.Allowed(folder)) { modsSaid = Install.TestOnly; ModsRefresh(true); return; }
        modsBusy = true;
        ModsRefresh();
        string? pick = mod?.Path, said = done;
        var bad = false;
        try
        {
            var to = await Task.Run(() => change(folder));
            Program.Log("mods: " + (done ?? "the order changed") + " (" + folder + ")");
            // (a mod with code patches changed before any build was marked: built in or not, the game is not as its mods are)
            if (mod is { Built: true } && GameFolder() is { } game && ModsBuilt(game) == null) Install.Set("ModsBuilt", game + "|changed");
            pick = to;
        }
        catch (ModsException e) { said = e.Message; bad = true; }
        catch (Exception e)
        {
            Program.Log("mods: " + e);
            said = "That did not work: " + e.Message;
            bad = true;
        }
        modsBusy = false;
        // (where the folder has another name now, the mod is found by its own)
        var now = await Task.Run(() => Mods.List(folder));
        pick ??= now.FirstOrDefault(m => mod != null && m.Name == mod.Name && string.Equals(System.IO.Path.GetDirectoryName(m.Path), System.IO.Path.GetDirectoryName(mod.Path), StringComparison.OrdinalIgnoreCase))?.Path;
        ModsFill(now, pick, said, bad);
    }

    void PickArchive()
    {
        if (FolderPicker.Answers.Count > 0) { AddMod(FolderPicker.Answers.Dequeue()); return; }   // (testing, TRIFORCE_UI_SCRIPT)
        var d = new OpenFileDialog { Filter = "Mods (*.zip, *.7z, *.rar)|*.zip;*.7z;*.rar", Title = "Choose the mod's archive" };
        if (d.ShowDialog(Window) == true) AddMod(d.FileName);
    }

    /// Add a mod: the archive is unpacked into the mods folder, checked there, and its mod moved into place (Mods.Add)
    async void AddMod(string archive)
    {
        var folder = ModsFolder();
        if (folder == null || modsBusy) return;
        if (!Install.Allowed(folder)) { modsSaid = Install.TestOnly; ModsRefresh(true); return; }
        modsBusy = true;
        modsSaid = "Unpacking " + System.IO.Path.GetFileName(archive) + "...";
        ModsRefresh();
        string? pick = PickedMod?.Path, said;
        var bad = false;
        try
        {
            var mod = await Task.Run(() => Mods.Add(folder, archive));
            Program.Log("mods: added " + mod.Rel + " from an archive (" + folder + ")");
            if (mod.Code && GameFolder() is { } game && ModsBuilt(game) == null) Install.Set("ModsBuilt", game + "|changed");
            pick = mod.Path;
            said = mod.Name + " was added: " + Mods.Carries(mod) + "." + (mod.Code ? " Its code patches and cheats take effect once the game is built again." : " It loads at the game's next start.");
        }
        catch (ModsException e) { said = e.Message; bad = true; }
        catch (Exception e)
        {
            Program.Log("mods: " + e);
            said = "The mod could not be added: " + e.Message;
            bad = true;
        }
        modsBusy = false;
        await LoadMods(pick, said, bad);
    }

    void OpenModsFolder()
    {
        var folder = ModsFolder();
        if (folder == null) return;
        try
        {
            if (!Directory.Exists(folder) && Install.Allowed(folder)) Directory.CreateDirectory(folder);
            if (scriptFile == null) Process.Start(new ProcessStartInfo("explorer.exe", "\"" + folder + "\"") { UseShellExecute = true });
        }
        catch (Exception e) { Program.Log("mods: the folder could not be opened: " + e.Message); }
    }

    /// Every file more than one mod carries, with its winner, onto the clipboard
    void CopyConflicts()
    {
        try
        {
            Clipboard.SetDataObject(Mods.ConflictText(conflicts), true);
            ModsConflicts.Content = "Copied";
        }
        catch (Exception e)
        {
            Program.Log("the conflicts could not be copied: " + e.Message);
            ModsConflicts.Content = T(S.ReportNotCopied);
        }
    }

    // ---- code patches and cheats: built in ----

    /// The mark of the mods' code the game folder was last built with (Mods.CodeState), null when Setup kept none for it
    static string? ModsBuilt(string game) =>
        Install.Get("ModsBuilt") is { } kept && kept.StartsWith(game + "|", StringComparison.OrdinalIgnoreCase) ? kept.Substring(game.Length + 1) : null;

    /// A build of this game folder ended well (Shell.cs Ended): the code patches and cheats of its mods are in it as they are now
    void MarkModsBuilt(string game)
    {
        try { Install.Set("ModsBuilt", game + "|" + Mods.CodeState(Mods.List(Mods.FolderOf(game)))); }
        catch (Exception e) { Program.Log("mods: the build's mark could not be kept: " + e.Message); }
    }

    /// "Build again to apply": the game folder built again from the game files it keeps, as an update is
    void BuildForMods()
    {
        var game = GameFolder();
        if (game == null) return;
        Ask("Build your game again with its mods' code patches? Only what changed is compiled again, so it usually takes a few minutes.", "Build again", T(S.Back), () =>
        {
            if (Tools.Find() is not { } tools) { modsSaid = T(S.ToolsMissing); ModsRefresh(true); return; }
            if (GameRuns(game)) { modsSaid = "Close the game first: it cannot be built again while it runs."; ModsRefresh(true); return; }
            OutPath.Text = game;
            BeginUpdate();
            Run(tools, new List<string> { "--update", game }, keepRows: true);
        });
    }
}
