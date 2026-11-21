// The game's in-game Options (gpu/vk/vk_menu.cpp, the Row table) as plain data for the launcher: one GameOptionRow per row the
// game keeps in triforce_options.ini, with its tab, section, name, key, range, default, help and the labels its value shows.
// GameOptionsSchema.All lists them; GameOptionsFile reads and writes the file the way the game does.
namespace Triforce.Logic;

/// <summary>The Options screen's tabs, as the game's header shows them. (The game's Debug tab is not ported: its rows'
/// keys are kept in the file as they are, GameOptionsSchema.KeptKeys.)</summary>
public enum GameOptionTab { Display, Graphics, Audio, Controls, System }

/// <summary>Choice: a few values, each with a label (toggles and lists). Slider: a numeric range on steps (volume, nits,
/// percent, sensitivity), each value labelled by its unit (Label).</summary>
public enum GameOptionKind { Choice, Slider }

/// <summary>One value of a row and the text the game's menu shows for it.</summary>
public sealed record GameOptionChoice(int Value, string Label);

/// <summary>The values of the other rows, for the labels that depend on them (the Render resolution's sizes follow the
/// Upscaler, the Output resolution and the Handheld profile; the Anti-aliasing row shows the upscaler's own while one
/// draws; the Microphone shows the saved device's name). GameOptionsFile is one.</summary>
public interface IGameOptionValues
{
    /// <summary>A row's value as the game reads it (its default when the file has no line).</summary>
    int Get(string key);
    /// <summary>A line's value as the file has it, null when it has no such line.</summary>
    string? Raw(string key);
}

/// <summary>A row of the game's Options menu that has a value in triforce_options.ini.</summary>
public sealed record GameOptionRow
{
    /// <summary>The tab it is on. The Motion page's rows and the Controls page's Button prompts are on Controls, in
    /// the sections "Motion" and "Controls" (the rows that open those pages).</summary>
    public required GameOptionTab Tab { get; init; }
    /// <summary>The heading above it on its tab (the menu's section rows), null where the tab has none (Audio).</summary>
    public required string? Section { get; init; }
    /// <summary>The row's name, exactly as the menu shows it.</summary>
    public required string Name { get; init; }
    /// <summary>Its key in triforce_options.ini (vk_menu.cpp ini_key).</summary>
    public required string Key { get; init; }
    public required int Lo { get; init; }
    public required int Hi { get; init; }
    public required int Step { get; init; }
    /// <summary>The game's default (vk_menu.h Options): the value when the file has no line for it.</summary>
    public required int Default { get; init; }
    public required GameOptionKind Kind { get; init; }
    /// <summary>The help under the list while the row is selected, exactly the menu's.</summary>
    public required string Help { get; init; }
    /// <summary>Read once at the game's start: a change applies after a restart (the menu tags it "After restart").</summary>
    public required bool Restart { get; init; }
    /// <summary>A change here leaves the Quality preset alone; any other row changed by hand turns it back to Custom.</summary>
    public required bool PresetExempt { get; init; }

    internal Func<int, IGameOptionValues?, string> Format { get; init; } = (v, _) => v.ToString(System.Globalization.CultureInfo.InvariantCulture);
    internal Func<IReadOnlyList<GameOptionChoice>>? ChoiceList { get; init; }

    /// <summary>The values a Choice row offers, in the menu's order, with their labels as the menu shows them where nothing
    /// is known of this PC (fps cap 6 "Monitor's rate", GPU device from GameOptionsSchema.GpuNames). Empty for a Slider.
    /// GameOptionsFile.ChoicesOf has the ones that follow the file's other rows (the Render resolution).</summary>
    public IReadOnlyList<GameOptionChoice> Choices => ChoiceList?.Invoke() ?? Array.Empty<GameOptionChoice>();

    /// <summary>The label of a value, as the menu shows it with the other rows at their defaults.</summary>
    public string Label(int value) => Format(value, null);

    /// <summary>The label of a value with the other rows as values has them.</summary>
    public string Label(int value, IGameOptionValues? values) => Format(value, values);

    /// <summary>A value as the launcher may set it: inside Lo..Hi, on the row's steps from Lo (the nearest, the larger on a
    /// tie). The Output resolution's values past its list are its first, as the game reads them.</summary>
    public int Clamp(int value)
    {
        if (Key == GameOptionsSchema.OutputResKey) return value >= Lo && value <= Hi ? value : 0;
        var v = Math.Clamp(value, Lo, Hi);
        if (Step > 1)
        {
            var k = (v - Lo + Step / 2) / Step;
            v = Math.Min(Lo + k * Step, Hi);
        }
        return v;
    }

    /// <summary>A value as the game reads it from the file (menu_init): inside Lo..Hi, not moved onto the steps (a hand-edited
    /// 125 stays 125), except the Rumble strength (the nearest 25) and the Output resolution (past its list: its first).</summary>
    public int AsRead(int value)
    {
        if (Key == GameOptionsSchema.OutputResKey) return value >= Lo && value <= Hi ? value : 0;
        var v = Math.Clamp(value, Lo, Hi);
        if (Key == GameOptionsSchema.RumbleKey) v = (Math.Clamp(v, 0, 300) + 12) / 25 * 25;
        return v;
    }
}
