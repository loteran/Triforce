// The launcher page's color scheme (Settings > Launcher > Color scheme): one color in place of Triforce gold. The page draws
// it (ui/app.js, "settings: Color scheme") and sends {t:'cmd', c:'color', hex:'#rrggbb'}; the launcher keeps it with its
// other settings (Settings.Color) and sends it back as state.color at start and when the Settings page asks for its list.
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Triforce.Logic;

public static class LauncherColor
{
    /// <summary>Triforce gold, the look as it was: the page's GOLD, and what Reset to gold sends.</summary>
    public const string Default = "#e0b23e";

    /// <summary>"#rrggbb" (six hex digits, either case) as "#rrggbb" in lower case; null for anything else (no "#", three
    /// digits, a name, spaces around it, an alpha).</summary>
    public static string? Parse(string? s)
    {
        if (s is not { Length: 7 } || s[0] != '#') return null;
        for (var i = 1; i < 7; i++)
            if (!char.IsAsciiHexDigit(s[i])) return null;
        return s.ToLowerInvariant();
    }

    /// <summary>The page's 'color' command: its "hex" when that is a JSON string Parse takes, else null (no object, no hex,
    /// a number, an array, an object, a bad string).</summary>
    public static string? FromCommand(JsonNode? message) =>
        message is JsonObject o && o["hex"] is JsonValue v && v.GetValueKind() == JsonValueKind.String ? Parse(v.GetValue<string>()) : null;
}

/// <summary>The launcher's color while it runs: the last one the page sent, else the kept one (Settings.Color). It is held here
/// and not only in the settings because a run that writes nothing (NoWrites, --snapshot) must still send back the color the
/// player chose, not the old kept one, and because the page's commands run on worker threads in any order: Take is called as
/// each 'color' arrives, in the order the page sent it, so a 'settings.list' handled after it sends the new color, and Keep
/// writes the newest choice whichever of its workers runs last.</summary>
public sealed class LauncherColorChoice
{
    readonly Settings settings;
    readonly object keeping = new();
    volatile string? chosen;

    public LauncherColorChoice(Settings settings) => this.settings = settings;

    /// <summary>What state.color says: the page's last color, or the kept one before the page has sent any.</summary>
    public string Current => chosen ?? settings.Color;

    /// <summary>The page's 'color' command, as it arrives: its color (LauncherColor.FromCommand) becomes Current; null when it
    /// is not a #rrggbb, and Current stays.</summary>
    public string? Take(JsonNode? message)
    {
        var hex = LauncherColor.FromCommand(message);
        if (hex != null) chosen = hex;
        return hex;
    }

    /// <summary>Writes Current as Settings.Color when the page has sent one and it differs (dropped under NoWrites).</summary>
    public void Keep()
    {
        lock (keeping)
            if (chosen is { } hex && hex != settings.Color) settings.Color = hex;
    }
}
