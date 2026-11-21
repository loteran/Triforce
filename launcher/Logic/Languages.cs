using System.Globalization;

namespace Triforce.Logic;

/// <summary>One of the game's languages: the code make_game.py takes (--language) and writes to language.txt, its name in English
/// (make_game.py LANGUAGES) and in itself (old Strings.cs).</summary>
public sealed record GameLanguage(string Code, string EnglishName, string NativeName);

public static class Languages
{
    /// <summary>The 16 codes, identical in make_game.py LANGUAGES (nx2/make_game.py:60-63) and the old Strings.cs All.</summary>
    public static readonly IReadOnlyList<GameLanguage> All = new GameLanguage[]
    {
        new("en-US", "English (US)", "English (US)"), new("en-GB", "English (UK)", "English (UK)"),
        new("fr", "French (France)", "Français"), new("fr-CA", "French (Canada)", "Français (Canada)"),
        new("de", "German", "Deutsch"), new("it", "Italian", "Italiano"),
        new("es", "Spanish (Spain)", "Español (España)"), new("es-419", "Spanish (Latin America)", "Español (Latinoamérica)"),
        new("pt-BR", "Portuguese (Brazil)", "Português (Brasil)"), new("nl", "Dutch", "Nederlands"),
        new("pl", "Polish", "Polski"), new("ru", "Russian", "Русский"),
        new("ja", "Japanese", "日本語"), new("ko", "Korean", "한국어"),
        new("zh-Hans", "Chinese (Simplified)", "简体中文"), new("zh-Hant", "Chinese (Traditional)", "繁體中文"),
    };

    public static GameLanguage? Find(string? code) => All.FirstOrDefault(l => l.Code == code);

    /// <summary>The language closest to the system's own (old Strings.cs Default): the exact one, then the same family, else
    /// English (US). name: a culture name ("de-AT"); the current UI culture's, or LANG's on Linux, when none is given.</summary>
    public static GameLanguage Default(string? name = null)
    {
        name ??= CultureInfo.CurrentUICulture.Name;
        if (string.IsNullOrEmpty(name) && Environment.GetEnvironmentVariable("LANG") is { Length: > 0 } lang)
            name = lang.Split('.')[0].Replace('_', '-');   // (InvariantGlobalization: the culture says nothing; LANG=de_AT.UTF-8 does)
        name ??= "";
        var pick = All.FirstOrDefault(l => l.Code.Equals(name, StringComparison.OrdinalIgnoreCase));
        if (pick != null) return pick;
        if (name.StartsWith("zh", StringComparison.OrdinalIgnoreCase))
            return Find(name.Contains("TW") || name.Contains("HK") || name.Contains("Hant") ? "zh-Hant" : "zh-Hans")!;
        if (name.StartsWith("es-", StringComparison.OrdinalIgnoreCase) && !name.Equals("es-ES", StringComparison.OrdinalIgnoreCase)) return Find("es-419")!;
        if (name.StartsWith("pt", StringComparison.OrdinalIgnoreCase)) return Find("pt-BR")!;
        var two = name.Split('-')[0].ToLowerInvariant();
        return All.FirstOrDefault(l => l.Code == two) ?? All[0];
    }
}
