// The launcher's test commands. One command per case below; others add theirs to the switch.
//
//   gamefiles <nup-or-folder> <outdir>   decode the title art and the sounds from a player's game, time it, and compare
//                                        with the reference files the lab's tools made
//   caption <caption.sav> <out.jpg>      the JPEG of a save slot's caption.sav
//
// Output made from the game goes only where it is told to (the lab's scratch folder), never into the repository.
using System.Diagnostics;
using Triforce.Core.GameFiles;

if (args.Length == 0)
{
    Console.WriteLine("usage: Triforce.Tests <command> ...   (commands: gamefiles, caption)");
    return 2;
}

switch (args[0])
{
    case "gamefiles":
        return args.Length == 3 ? GameFilesTest.Run(args[1], args[2]) : Usage("gamefiles <nup-or-folder> <outdir>");
    case "caption":
        return args.Length == 3 ? GameFilesTest.CaptionTest(args[1], args[2]) : Usage("caption <caption.sav> <out.jpg>");
    default:
        Console.WriteLine("unknown command " + args[0]);
        return 2;
}

static int Usage(string text)
{
    Console.WriteLine("usage: Triforce.Tests " + text);
    return 2;
}

static class GameFilesTest
{
    const string ReferenceImages = @"D:\src\lab\hookshot\ui\launcher-rebuild\mockups\img\game";
    const string ReferenceAudio = @"D:\src\lab\hookshot\ui\launcher-rebuild\prototype\audio";

    static double Ms(Stopwatch w) => w.Elapsed.TotalMilliseconds;

    public static int Run(string game, string outDir)
    {
        Directory.CreateDirectory(outDir);
        var cache = Path.Combine(outDir, "cache");
        if (Directory.Exists(cache))
            Directory.Delete(cache, true);
        var failures = 0;

        // ---- open ----
        var w = Stopwatch.StartNew();
        using var source = GameSource.Open(game);
        Console.WriteLine("open: {0:0.0} ms  {1}", Ms(w), source.Describe);
        if (source is NupSource nup)
            foreach (var t in nup.Titles)
                Console.WriteLine("  title {0} v{1}", t.IdHex, t.Version);
        w.Restart();
        var exists = source.Exists(GameMedia.TitleArchive);
        Console.WriteLine("first lookup: {0:0.0} ms (title archive {1})", Ms(w), exists ? "found" : "MISSING");
        w.Restart();
        var streams = source.List("Sound/Resource/Stream").Count();
        Console.WriteLine("list Sound/Resource/Stream: {0} entries in {1:0.0} ms", streams, Ms(w));

        // ---- cold: what a player waits for on the first start ----
        var media = new GameMedia(source, cache);
        w.Restart();
        foreach (var name in GameMedia.LogoSet)
        {
            var one = Stopwatch.StartNew();
            media.PngFile(name);
            Console.WriteLine("  {0,-20} {1,7:0.0} ms", name, Ms(one));
        }
        Console.WriteLine("cold: logo set (4 PNGs) {0:0.0} ms", Ms(w));
        w.Restart();
        media.WavFile("saria");
        Console.WriteLine("cold: Saria's Song (8 layers mixed) {0:0.0} ms", Ms(w));
        w.Restart();
        foreach (var id in GameMedia.Sounds.Keys.Where(k => k != "saria"))
            media.WavFile(id);
        Console.WriteLine("cold: the other 13 sounds {0:0.0} ms", Ms(w));

        // ---- warm: a second start (a new instance, same cache) ----
        var again = new GameMedia(source, cache);
        w.Restart();
        foreach (var name in GameMedia.LogoSet)
            again.Png(name);
        Console.WriteLine("warm: logo set {0:0.0} ms", Ms(w));
        w.Restart();
        again.Wav("saria");
        Console.WriteLine("warm: Saria's Song {0:0.0} ms", Ms(w));

        // ---- where the logo's time goes (decoded again, outside the cache) ----
        w.Restart();
        var zs = new Zs(source);
        _ = zs.Dictionaries.Count;
        Console.WriteLine("  breakdown: dictionaries {0:0.0} ms", Ms(w));
        w.Restart();
        var archive = new Bntx(zs.Unpack(GameMedia.TitleArchive));
        Console.WriteLine("  breakdown: title archive read + unpacked + parsed {0:0.0} ms", Ms(w));
        var logo = archive.Find("Logo_USEU_00")!;
        w.Restart();
        var rgba = archive.DecodeRgba(logo);
        Console.WriteLine("  breakdown: Logo_USEU_00 deswizzle + ASTC {0:0.0} ms", Ms(w));
        w.Restart();
        Png.Encode(rgba, logo.Width, logo.Height);
        Console.WriteLine("  breakdown: Logo_USEU_00 PNG {0:0.0} ms", Ms(w));

        // ---- everything at once from several threads, into a fresh cache (a page asking for all of it) ----
        var cache2 = Path.Combine(outDir, "cache-parallel");
        if (Directory.Exists(cache2))
            Directory.Delete(cache2, true);
        var together = new GameMedia(source, cache2);
        var jobs = GameMedia.LogoSet.Select(n => (Action)(() => together.Png(n)))
            .Concat(GameMedia.Sounds.Keys.Select(id => (Action)(() => together.Wav(id))))
            .ToList();
        jobs.AddRange(jobs.ToList()); // (each item asked for twice at the same time)
        w.Restart();
        Parallel.Invoke(jobs.ToArray());
        var made = Directory.GetFiles(cache2).Length;
        Console.WriteLine("cold, all at once from {0} threads: {1:0.0} ms, {2} cache files (18 expected)", jobs.Count, Ms(w), made);
        if (made != 18)
            failures++;

        // ---- the texture list (formats in the title archive) ----
        var formats = media.Textures().GroupBy(t => t.FormatName).Select(g => g.Key + " x" + g.Count());
        Console.WriteLine("title archive formats: " + string.Join(", ", formats));

        // ---- write and compare ----
        Console.WriteLine();
        Console.WriteLine("images (against {0}):", ReferenceImages);
        foreach (var name in GameMedia.LogoSet)
        {
            var png = media.Png(name);
            File.WriteAllBytes(Path.Combine(outDir, name + ".png"), png);
            var mine = Png.Decode(png);
            var refPath = Path.Combine(ReferenceImages, name + ".png");
            if (!File.Exists(refPath))
            {
                Console.WriteLine("  {0,-20} {1}x{2}  (no reference)", name, mine.Width, mine.Height);
                continue;
            }
            var reference = Png.Decode(File.ReadAllBytes(refPath));
            if (reference.Width != mine.Width || reference.Height != mine.Height)
            {
                Console.WriteLine("  {0,-20} SIZE DIFFERS: {1}x{2}, reference {3}x{4}", name, mine.Width, mine.Height, reference.Width, reference.Height);
                failures++;
                continue;
            }
            int maxDiff = 0, pixelsDiffering = 0;
            for (var i = 0; i < mine.Rgba.Length; i += 4)
            {
                var d = 0;
                for (var c = 0; c < 4; c++)
                    d = Math.Max(d, Math.Abs(mine.Rgba[i + c] - reference.Rgba[i + c]));
                if (d > 0)
                    pixelsDiffering++;
                maxDiff = Math.Max(maxDiff, d);
            }
            Console.WriteLine("  {0,-20} {1}x{2}  max channel difference {3}, {4} of {5} pixels differ  ({6} KB)", name, mine.Width,
                mine.Height, maxDiff, pixelsDiffering, mine.Width * mine.Height, png.Length / 1024);
            if (maxDiff > 1)
                failures++;
        }

        Console.WriteLine();
        Console.WriteLine("sounds (against {0}):", ReferenceAudio);
        foreach (var id in GameMedia.Sounds.Keys)
        {
            var wav = media.Wav(id);
            File.WriteAllBytes(Path.Combine(outDir, id + ".wav"), wav);
            var mine = Bwav.FromWav(wav);
            var refPath = Path.Combine(ReferenceAudio, id + ".wav");
            if (!File.Exists(refPath))
            {
                Console.WriteLine("  {0,-9} {1} frames  (no reference)", id, mine.Frames);
                continue;
            }
            var reference = Bwav.FromWav(File.ReadAllBytes(refPath));
            int maxDiff = 0, differing = 0;
            var n = Math.Min(mine.Samples.Length, reference.Samples.Length);
            for (var i = 0; i < n; i++)
            {
                var d = Math.Abs(mine.Samples[i] - reference.Samples[i]);
                if (d > 0)
                    differing++;
                maxDiff = Math.Max(maxDiff, d);
            }
            var lengthOk = mine.Frames == reference.Frames && mine.Rate == reference.Rate;
            Console.WriteLine("  {0,-9} {1,8} frames {2,6:0.00} s  reference {3,8} frames{4}  peak {5}/{6}  max sample difference {7}, {8} samples differ",
                id, mine.Frames, mine.Seconds, reference.Frames, lengthOk ? "" : " LENGTH DIFFERS", mine.Peak, reference.Peak, maxDiff, differing);
            if (!lengthOk || maxDiff > 1)
                failures++;
        }

        Console.WriteLine();
        Console.WriteLine(failures == 0 ? "all match" : failures + " item(s) differ");
        return failures == 0 ? 0 : 1;
    }

    public static int CaptionTest(string captionSav, string outJpg)
    {
        var jpeg = Caption.JpegFromFile(captionSav);
        if (jpeg == null)
        {
            Console.WriteLine("no JPEG in " + captionSav);
            return 1;
        }
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(outJpg))!);
        File.WriteAllBytes(outJpg, jpeg);
        Console.WriteLine("{0}: JPEG of {1} bytes -> {2}", captionSav, jpeg.Length, outJpg);
        return 0;
    }
}
