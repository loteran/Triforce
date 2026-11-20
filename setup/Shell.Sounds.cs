// (Triforce) Where Setup sounds: the game's own decide sound when a button, a link or a row of a list is chosen or a choice
// changes, its cancel sound when the player goes back, cancels or answers no (Sound.cs has the sounds and where they come
// from: the player's own game, never a file of Setup's). Only what the player does sounds: a page the programme turns, a
// list it fills, the cursor moving over things do not. And nothing sounds in a run without a player (--snapshot, a script,
// a test folder) or while the game runs.
//
// The setting is "Sounds" at the window's top, beside the minimize button: a click goes on to the next of Off, Low (as it
// starts), Medium and High, and plays the decide sound at it. Kept with Setup's other settings (Sounds in HKCU\Software\Triforce).
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;

namespace TriforceSetup;

sealed partial class Shell
{
    Sounds sounds = null!;
    Button SoundsButton = null!;
    Caps SoundsLabel = null!;
    bool byHand;   // (inside the player's own click or key on a list: a choice that changes now is theirs)
    bool gameUp;
    bool playing;   // ((Triforce) Play was chosen: its start sound plays, no click's over it
    int gameAsked = Environment.TickCount - 60000;

    /// The buttons that go back, cancel or say no: the cancel sound. Every other button and link: the decide sound
    static readonly HashSet<string> GoBack = new() { "SavesBack", "ModsBack", "SetupBack", "BackButton", "AskNo", "FetchCancel", "CancelButton", "NotNowButton" };

    void InitSounds()
    {
        SoundsButton = Part<Button>("SoundsButton");
        SoundsLabel = Part<Caps>("SoundsLabel");
        var scripted = Environment.GetEnvironmentVariable("TRIFORCE_UI_SCRIPT") is { Length: > 0 };
        sounds = new Sounds(Sounds.PlayerFor(snapshot != null, scripted, Install.TestRoot, Environment.GetEnvironmentVariable("TRIFORCE_SOUND_LOG")), GameSounds, Install.Get("Sounds"));
        ShowSoundsLevel();
        SoundsButton.Click += (_, _) =>
        {
            sounds.Level = Sounds.Next(sounds.Level);
            Install.Set("Sounds", sounds.Level);
            ShowSoundsLevel();
            sounds.Play(Sounds.Decide);   // (how loud it is now)
        };
        // (handledEventsToo: a button's own handler has run by now, and may have marked the click as handled)
        Window.AddHandler(ButtonBase.ClickEvent, new RoutedEventHandler(SoundOfClick), true);
        Window.AddHandler(Selector.SelectionChangedEvent, new SelectionChangedEventHandler(SoundOfChoice), true);
        Window.PreviewMouseDown += (_, e) => ByHand(e.OriginalSource);
        Window.PreviewMouseUp += (_, e) => ByHand(e.OriginalSource);
        Window.PreviewKeyDown += (_, e) =>
        {
            ByHand(Keyboard.FocusedElement);
            // (Esc answers a question with no: Shell.cs Wire, whose handler comes after this one and hides the question)
            if (e.Key == Key.Escape && AskLayer.Visibility == Visibility.Visible) sounds.Play(Sounds.Cancel);
        };
        Window.Loaded += (_, _) => LoadSounds();
    }

    void ShowSoundsLevel()
    {
        var level = sounds.Level;
        SoundsLabel.Label = "Sounds · " + char.ToUpperInvariant(level[0]) + level.Substring(1);
        SoundsButton.ToolTip = level == "off" ? "Setup is silent. Click for the game's own menu sounds, low"
            : "Setup plays your game's own menu sounds (none before a game is built). Click to change how loud";
    }

    /// The game's two sounds taken up from the game folder there is now, off the window's thread: at the start, and wherever
    /// the game folder or its mods may have changed (the welcome-back page, the Mods page's list, a build that ended)
    void LoadSounds()
    {
        var game = GameFolder();
        Task.Run(() => sounds.Load(game));
    }

    /// Does the game run? Asked of Windows at most every two seconds. (In a test run only a game inside the test folder
    /// counts, as for the saves and the mods: the PC's own game has nothing to do with it)
    bool GameSounds()
    {
        var now = Environment.TickCount;
        if (unchecked(now - gameAsked) < 2000) return gameUp;
        gameAsked = now;
        return gameUp = Install.TestRoot != null ? new[] { "Triforce", "hookshot" }.Any(name => Running(name, Install.TestRoot, false)) : Sounds.GameProcess();
    }

    static bool Inside<T>(object? from) where T : DependencyObject
    {
        for (var at = from as DependencyObject; at != null; at = at is Visual ? VisualTreeHelper.GetParent(at) : LogicalTreeHelper.GetParent(at))
            if (at is T) return true;
        return false;
    }

    /// A press or a key on a row of a list (or of the language's list): until this input has been dealt with, a choice that
    /// changes is the player's. One the programme makes later (a list filled anew) is not
    void ByHand(object? on)
    {
        if (!Inside<ListBoxItem>(on) && !Inside<ComboBoxItem>(on) && !Inside<ComboBox>(on)) return;
        byHand = true;
        Window.Dispatcher.BeginInvoke(DispatcherPriority.Send, new Action(() => byHand = false));
    }

    void SoundOfChoice(object sender, SelectionChangedEventArgs e)
    {
        if (byHand && e.AddedItems.Count > 0 && e.OriginalSource is ListBox or ComboBox) sounds.Play(Sounds.Decide);
    }

    void SoundOfClick(object sender, RoutedEventArgs e)
    {
        // (the window's own buttons, links, ticks and segments: not the parts of a scroll bar or of the language's list, and
        // not minimize and close; the Sounds setting plays its own)
        if (e.OriginalSource is not ButtonBase { Name: { Length: > 0 } name } button || button is RepeatButton || button.TemplatedParent != null) return;
        if (button == SoundsButton || name == "MinButton" || name == "CloseButton") return;
        if (playing) return;   // ((Triforce) Play's own sound plays: Shell.cs Play)
        sounds.Play(GoBack.Contains(name) ? Sounds.Cancel : Sounds.Decide);
    }
}
