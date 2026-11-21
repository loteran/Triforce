using System.Text.Json.Nodes;
using Triforce.Core.GameFiles;

namespace Triforce.Launcher;

/// <summary>The player's own game as the page's art and sound: the title logo, Saria's Song and the menu sounds, read from
/// the .nup they dropped (or, once built, from the game folder's romfs) and kept decoded in the launcher's cache.</summary>
public sealed partial class Backend
{
    GameMedia? media;
    string? mediaFrom;
    int mediaVersion;

    byte[]? MediaFor(string kind, string name)
    {
        var m = media;
        if (m == null) return null;
        return kind == "png" ? m.Png(name) : kind == "wav" ? m.Wav(name) : null;
    }

    /// <summary>Use this game for the art and sounds (null: none). Decodes the logo set and the first music right away, so
    /// the reveal shows them at once.</summary>
    bool UseMedia(string? path)
    {
        if (path == mediaFrom && media != null) return true;
        if (path == null)
        {
            var gone = media; media = null; mediaFrom = null; gone?.Source.Dispose();
            SendMedia();
            return false;
        }
        try
        {
            var src = GameSource.Open(path);
            var m = new GameMedia(src, Path.Combine(dataDir, "media"));
            var t = System.Diagnostics.Stopwatch.StartNew();
            foreach (var n in GameMedia.LogoSet) m.PngFile(n);
            m.WavFile("saria");
            Log.Write($"media from {path}: logo + Saria's Song ready in {t.ElapsedMilliseconds} ms");
            var old = media;
            media = m; mediaFrom = path; mediaVersion++;
            old?.Source.Dispose();
            // the rest in the background
            _ = Task.Run(() => { try { foreach (var id in GameMedia.Sounds.Keys) m.WavFile(id); } catch (Exception e) { Log.Write("media: " + e.Message); } });
            SendMedia();
            return true;
        }
        catch (Exception e)
        {
            // (a source without the game's files: keep what is shown now)
            Log.Write("media from " + path + ": " + e.Message);
            if (media == null) SendMedia();
            return false;
        }
    }

    void SendMedia()
    {
        var on = media != null;
        Send(new JsonObject { ["t"] = "state", ["media"] = new JsonObject { ["art"] = on, ["music"] = on, ["sfx"] = on, ["v"] = mediaVersion } });
    }
}
