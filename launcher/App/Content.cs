using System.Reflection;

namespace Triforce.Launcher;

/// <summary>What the page asks the window for, by path under the origin:
///   ui/...            the page itself (embedded in the exe)
///   media/png/NAME    a texture from the player's own game (Core/GameFiles), e.g. the title logo
///   media/wav/ID      music or a sound from the player's own game
///   save/N.jpg        the picture in save file N (the game's own caption.sav)
/// Anything not there answers 404, and the page falls back (no game yet: no game art, no music).</summary>
public sealed class Content
{
    public delegate byte[]? MediaSource(string kind, string name);

    readonly Assembly asm = typeof(Content).Assembly;
    readonly Dictionary<string, string> names;
    public MediaSource? Media;
    public Func<int, byte[]?>? SavePicture;

    public Content()
    {
        // Resource names come out as "ui/img\crest.png" on Windows builds: make them all forward slashes.
        names = asm.GetManifestResourceNames().ToDictionary(n => n.Replace('\\', '/'), n => n, StringComparer.OrdinalIgnoreCase);
    }

    public (byte[] data, string mime)? Get(string path)
    {
        path = Uri.UnescapeDataString(path.Split('?', '#')[0]).TrimStart('/');
        // The page names the game's art and sounds as its own files (as the prototype did); they come from the game.
        if (path.StartsWith("ui/img/game/", StringComparison.Ordinal)) path = "media/png/" + path["ui/img/game/".Length..];
        else if (path.StartsWith("ui/audio/", StringComparison.Ordinal)) path = "media/wav/" + path["ui/audio/".Length..];
        else if (path.StartsWith("ui/img/save", StringComparison.Ordinal) && path.EndsWith(".jpg")) path = "save/" + path["ui/img/save".Length..];
        try
        {
            if (path.StartsWith("ui/", StringComparison.Ordinal))
            {
                if (!names.TryGetValue(path, out var res)) return null;
                using var s = asm.GetManifestResourceStream(res)!;
                using var m = new MemoryStream();
                s.CopyTo(m);
                return (m.ToArray(), Mime(path));
            }
            if (path.StartsWith("media/", StringComparison.Ordinal))
            {
                var parts = path.Split('/');
                if (parts.Length != 3 || Media == null) return null;
                var name = Path.GetFileNameWithoutExtension(parts[2]);
                var data = Media(parts[1], name);
                return data == null ? null : (data, parts[1] == "png" ? "image/png" : "audio/wav");
            }
            if (path.StartsWith("save/", StringComparison.Ordinal) && SavePicture != null &&
                int.TryParse(Path.GetFileNameWithoutExtension(path)[..Math.Min(2, Path.GetFileNameWithoutExtension(path).Length)], out var n))
            {
                var j = SavePicture(n);
                return j == null ? null : (j, "image/jpeg");
            }
        }
        catch (Exception e)
        {
            Log.Write("content " + path + ": " + e.Message);
        }
        return null;
    }

    static string Mime(string p) => Path.GetExtension(p).ToLowerInvariant() switch
    {
        ".html" => "text/html; charset=utf-8",
        ".css" => "text/css; charset=utf-8",
        ".js" => "text/javascript; charset=utf-8",
        ".png" => "image/png",
        ".jpg" or ".jpeg" => "image/jpeg",
        ".svg" => "image/svg+xml",
        ".woff2" => "font/woff2",
        ".ttf" => "font/ttf",
        ".wav" => "audio/wav",
        ".json" => "application/json",
        _ => "application/octet-stream",
    };
}
