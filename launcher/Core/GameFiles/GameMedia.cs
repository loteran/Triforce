// What the launcher shows and plays from the player's own game: the title art as PNG and the music and interface sounds as
// WAV, decoded at run time from their .nup or game folder (DESIGN.md section 10 lists the sounds and where each plays).
// Results are kept as files in a cache folder the caller gives (in the user's own app data, never the repository or the
// install folder), keyed by a cheap identity of the source files, so the second start of the launcher reads them back at
// once and a changed game file (another dump, an update, a mod) is decoded again.
using System.Collections.Concurrent;
using System.Security.Cryptography;
using System.Text;

namespace Triforce.Core.GameFiles;

/// <summary>The game's art and audio for the launcher, decoded on first use and cached. Thread-safe: two calls for the
/// same item decode it once; different items decode side by side.</summary>
public sealed class GameMedia
{
    /// <summary>Changes whenever a decoder's output would change, so older cache files are not used.</summary>
    const string CacheVersion = "gamemedia-1";

    /// <summary>The same for the sounds alone (the art's files keep their names): "loop-1" since the WAV files carry the
    /// game's loop in a smpl chunk, so a cache made before is decoded again once, its old files removed.</summary>
    const string SoundVersion = CacheVersion + "+loop-1";

    /// <summary>The title screen's texture archive.</summary>
    public const string TitleArchive = "UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.bntx.zs";

    /// <summary>The textures the launcher shows (the logo set): the logo, its aura, the button ornament and the logo's glow.</summary>
    public static readonly IReadOnlyList<string> LogoSet = new[] { "Logo_USEU_00", "TitleLogoAura_00", "TitleBtnDeco_00", "Logo_GlowL_USEU_00" };

    /// <summary>The sounds by the launcher's name for them, each the game's stream file(s) mixed (DESIGN.md section 10).</summary>
    public static readonly IReadOnlyDictionary<string, string[]> Sounds = new Dictionary<string, string[]>
    {
        // music
        ["saria"] = Enumerable.Range(0, 8).Select(i => "BGM_LostWood_" + i).ToArray(), // the 8 layers at once: the whole song
        ["fairy"] = new[] { "BGM_GreatFairyFountain" },
        ["temple"] = new[] { "BGM_ShrineOfTime_Child" },
        ["itemget"] = new[] { "BGM_ItemGet_Large" },
        // interface sounds
        ["cursor"] = new[] { "SE_Sys_SysMenu_Cursor00" },
        ["tab_l"] = new[] { "SE_Sys_MinusMenu_TabMove_L00" },
        ["tab_r"] = new[] { "SE_Sys_MinusMenu_TabMove_R00" },
        ["decide"] = new[] { "SE_Sys_SysMenu_DecideS00" },
        ["decide_l"] = new[] { "SE_Sys_SysMenu_DecideL00" },
        ["cancel"] = new[] { "SE_Sys_Cancel00" },
        ["open"] = new[] { "SE_Sys_PlusMenu_Open00" },
        ["close"] = new[] { "SE_Sys_PlusMenu_Close00" },
        ["success"] = new[] { "SE_Sys_OcarinaWindow_Success00" },
        ["error"] = new[] { "SE_Sys_SysMenu_Error00" },
    };

    /// <summary>A stream file's romfs path.</summary>
    public static string StreamPath(string name) => "Sound/Resource/Stream/" + name + ".bwav";

    readonly IGameSource source;
    readonly string cacheDir;
    readonly Zs zs;
    readonly ConcurrentDictionary<string, Lazy<Bntx>> archives = new(StringComparer.Ordinal);
    // (one lock per cache file, for the whole process: two instances on the same cache folder decode an item once)
    static readonly ConcurrentDictionary<string, object> gates = new(StringComparer.OrdinalIgnoreCase);

    public GameMedia(IGameSource source, string cacheDir)
    {
        this.source = source;
        this.cacheDir = Path.GetFullPath(cacheDir);
        zs = new Zs(source);
    }

    public IGameSource Source => source;

    /// <summary>A texture of the title archive as a PNG file's bytes.</summary>
    public byte[] Png(string textureName) => Png(TitleArchive, textureName);

    /// <summary>A texture of any BNTX archive in the romfs (a .bntx.zs or a plain .bntx) as a PNG file's bytes.</summary>
    public byte[] Png(string archive, string textureName) => File.ReadAllBytes(PngFile(archive, textureName));

    /// <summary>A sound (one of <see cref="Sounds"/>' names) as a WAV file's bytes.</summary>
    public byte[] Wav(string id) => File.ReadAllBytes(WavFile(id));

    /// <summary>The cache file holding the texture's PNG, decoded first when it is not there (for a page that loads it by
    /// path or URL).</summary>
    public string PngFile(string archive, string textureName) =>
        Cached(Safe(textureName) + ".png", new[] { archive, Zs.DictionaryPack }, () =>
        {
            var bntx = Archive(archive);
            var texture = bntx.Find(textureName)
                ?? throw new KeyNotFoundException("The game's " + archive + " has no texture " + textureName);
            var rgba = bntx.DecodeRgba(texture);
            return GameFiles.Png.Encode(rgba, texture.Width, texture.Height);
        });

    public string PngFile(string textureName) => PngFile(TitleArchive, textureName);

    /// <summary>The cache file holding the sound's WAV, decoded first when it is not there.</summary>
    public string WavFile(string id)
    {
        if (!Sounds.TryGetValue(id, out var names))
            throw new KeyNotFoundException("No sound called " + id + " (known: " + string.Join(", ", Sounds.Keys) + ")");
        var paths = names.Select(StreamPath).ToArray();
        return Cached(Safe(id) + ".wav", paths, () =>
        {
            // (the layers of a mixed song are read and decoded side by side)
            var tracks = new Pcm[paths.Length];
            Parallel.For(0, paths.Length, i => tracks[i] = Bwav.Decode(source.Read(paths[i])));
            return Bwav.ToWav(Bwav.Mix(tracks));
        }, SoundVersion);
    }

    /// <summary>The names of the textures in an archive (to check a name, or to pick another piece of art).</summary>
    public IReadOnlyList<BntxTexture> Textures(string archive = TitleArchive) => Archive(archive).Textures;

    /// <summary>Lets go of the unpacked archives kept for further textures (about 30 MB for the title archive).</summary>
    public void Trim() => archives.Clear();

    Bntx Archive(string archive) => archives.GetOrAdd(archive, a => new Lazy<Bntx>(() =>
        new Bntx(a.EndsWith(".zs", StringComparison.Ordinal) ? zs.Unpack(a) : source.Read(a)))).Value;

    /// <summary>
    /// The cache file for an item: "&lt;name&gt;-&lt;key&gt;.&lt;ext&gt;", the key a hash of the decoders' version and the
    /// source files' stamps. Made by `make` when it is not there (written beside and then moved into place, so a reader
    /// never sees half a file); other versions of the same item are removed then.
    /// </summary>
    string Cached(string fileName, string[] inputs, Func<byte[]> make, string version = CacheVersion)
    {
        var identity = new StringBuilder(version).Append('|').Append(fileName);
        foreach (var input in inputs)
            identity.Append('|').Append(input).Append('=').Append(source.Stamp(input)
                ?? throw new FileNotFoundException("The game has no " + input + " (" + source.Describe + ")", input));
        var key = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(identity.ToString())), 0, 8).ToLowerInvariant();
        var stem = Path.GetFileNameWithoutExtension(fileName);
        var ext = Path.GetExtension(fileName);
        var path = Path.Combine(cacheDir, stem + "-" + key + ext);

        lock (gates.GetOrAdd(path, _ => new object()))
        {
            if (File.Exists(path))
                return path;
            var bytes = make();
            Directory.CreateDirectory(cacheDir);
            var part = path + "." + Environment.ProcessId + ".part";
            File.WriteAllBytes(part, bytes);
            File.Move(part, path, overwrite: true);
            foreach (var old in Directory.EnumerateFiles(cacheDir, stem + "-*" + ext))
                if (!string.Equals(old, path, StringComparison.OrdinalIgnoreCase) && Path.GetFileName(old).Length == Path.GetFileName(path).Length)
                    try { File.Delete(old); } catch (IOException) { } catch (UnauthorizedAccessException) { }
            return path;
        }
    }

    /// <summary>A name made safe as a file name.</summary>
    static string Safe(string name)
    {
        var chars = name.Select(c => char.IsAsciiLetterOrDigit(c) || c is '_' or '.' ? c : '_').ToArray();
        return new string(chars);
    }
}
