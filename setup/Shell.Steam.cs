// (Triforce) Add to Steam, the link beside "Your game" on the welcome-back page: the game as a shortcut in the player's Steam
// library (Steam.cs), with pictures drawn from Setup's own crest (SteamArt.cs). Steam's files are another program's: they are
// looked at when the link is clicked, and written only from the question the player then answers.
using System;
using System.Linq;
using System.Windows.Controls;

namespace TriforceSetup;

sealed partial class Shell
{
    Button SteamButton = null!;

    void InitSteam()
    {
        SteamButton = Part<Button>("SteamButton");
        // (a test run has its own Steam folder, which the PC's Steam, running or not, has nothing to do with)
        if (Install.TestRoot != null) Steam.Running = root => Running("steam", root, false);
        SteamButton.Click += (_, _) => SteamClicked();
    }

    /// Steam's folder: the one Steam names in the registry. A test run has only the Steam folder inside its own folder,
    /// and never asks for the PC's
    static string? SteamRoot()
    {
        var root = Install.TestRoot != null ? System.IO.Path.Combine(Install.TestRoot, "Steam") : Steam.FromRegistry();
        return root != null && Steam.Valid(root) ? root : null;
    }

    /// A notice over the whole window, with one button
    void Tell(string text) => Ask(text, "OK", "", () => { });

    /// The link beside "Your game": what is there is looked at now, and the player is asked before anything is written. For
    /// the account Steam was last signed in with, named in the question
    void SteamClicked()
    {
        var folder = Install.Get("GameFolder");
        var exe = folder != null ? GameExe(folder) : null;
        if (exe == null) return;
        try
        {
            var root = SteamRoot();
            if (root == null) { Tell("Steam was not found on this PC."); return; }
            var user = Steam.Users(root).FirstOrDefault();
            if (user == null) { Tell("Nobody has signed in to Steam on this PC yet. Start Steam, sign in once, close it, then try again."); return; }
            if (!Install.Allowed(user.Config)) { Tell(Install.TestOnly); return; }
            var whose = user.Name.Length > 0 ? user.Name + "'s Steam library" : "your Steam library";
            if (Steam.Running(root)) { Tell(Steam.CloseFirst); return; }
            var there = Steam.Where(user, exe);
            if (there != null && string.Equals(there, exe, StringComparison.OrdinalIgnoreCase))
                Ask("Triforce is in " + whose + ". Remove it from there?", "Remove from Steam", T(S.Back), () => SteamWrite(user, exe, whose, false));
            else if (there != null)
                Ask("Triforce is in " + whose + ", for the game in " + System.IO.Path.GetDirectoryName(there) + ". Set it to this game folder?", "Set to this folder", T(S.Back), () => SteamWrite(user, exe, whose, true));
            else
                Ask("Add Triforce to " + whose + "? Steam then starts the game itself, without Setup.", "Add to Steam", T(S.Back), () => SteamWrite(user, exe, whose, true));
        }
        catch (Exception e)
        {
            Program.Log("steam: " + e.Message);
            Tell("Steam's folder could not be read: " + e.Message);
        }
    }

    /// The player said yes: the shortcut and its pictures written into the account's config folder, or taken out of it
    void SteamWrite(SteamUser user, string exe, string whose, bool add)
    {
        try
        {
            if (!Install.Allowed(user.Config)) throw new SteamException(Install.TestOnly);
            var changed = add ? Steam.Add(user, exe, SteamArt.Make()) : Steam.Remove(user, exe);
            // (no account's name or number in the log: it is pasted where help is asked for)
            Program.Log("steam: the shortcut was " + (add ? changed ? "added" : "set to " + exe : "removed") + " for the account Steam was last signed in with");
            SteamButton.Content = add ? "Remove from Steam" : "Add to Steam";
            Tell(add ? "Triforce is in " + whose + " now, among the non-Steam games. Start Steam to see it."
                     : "Triforce is no longer in " + whose + ". The file as it was before is kept beside it (shortcuts.vdf" + Steam.Backup + ").");
        }
        catch (SteamException e) { Tell(e.Message); }
        catch (Exception e)
        {
            Program.Log("steam: " + e);
            Tell("Steam's shortcuts could not be changed: " + e.Message);
        }
    }
}
