// (Triforce) The pictures of the game's Steam shortcut (Steam.cs), drawn here from Setup's own crest (assets/triforce.png, the
// one picture in Setup: Art.cs) in the hall the window shows it in: near-black, one warm light, the lattice in it. Nothing is
// fetched and nothing is taken from the game. --snapshot writes them out to look at (Shell.Test.cs).
//   portrait   the library's cover: the crest over the name
//   wide       the wide tile: the same, lower
//   hero       the background of the game's page in the library: the hall with the crest to the right, no words (Steam puts
//              the logo over it)
//   logo       the crest and the name on nothing, for over the hero
//   icon       the crest alone
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace TriforceSetup;

static class SteamArt
{
    static readonly Typeface Serif = new(new FontFamily("Palatino Linotype, Book Antiqua, Georgia"), FontStyles.Normal, FontWeights.Normal, FontStretches.Normal);

    /// Each picture of Steam.Pictures as a PNG file's bytes, in the size listed there
    public static Dictionary<string, byte[]> Make()
    {
        if (Art.Crest == null) Art.Load();
        var made = new Dictionary<string, byte[]>();
        foreach (var (kind, _, width, height) in Steam.Pictures)
        {
            var picture = new DrawingVisual();
            RenderOptions.SetBitmapScalingMode(picture, BitmapScalingMode.HighQuality);
            using (var dc = picture.RenderOpen()) Draw(dc, kind, width, height);
            var bitmap = new RenderTargetBitmap(width, height, 96, 96, PixelFormats.Pbgra32);
            bitmap.Render(picture);
            bitmap.Freeze();
            var png = new PngBitmapEncoder();
            png.Frames.Add(BitmapFrame.Create(bitmap));
            using var bytes = new MemoryStream();
            png.Save(bytes);
            made[kind] = bytes.ToArray();
        }
        return made;
    }

    static void Draw(DrawingContext dc, string kind, double w, double h)
    {
        switch (kind)
        {
            case "portrait":
                Hall(dc, w, h, w / 2, h * 0.40, w * 0.95);
                Crest(dc, w / 2, h * 0.40, w * 0.62);
                Rule(dc, w / 2, h * 0.705, w * 0.36);
                Name(dc, w / 2, h * 0.775, w * 0.105, false);
                break;
            case "wide":
                Hall(dc, w, h, w / 2, h * 0.42, h * 1.25);
                Crest(dc, w / 2, h * 0.40, h * 0.60);
                Name(dc, w / 2, h * 0.83, h * 0.125, false);
                break;
            case "hero":
                Hall(dc, w, h, w * 0.74, h * 0.50, h * 1.35);
                Crest(dc, w * 0.74, h * 0.50, h * 0.74);
                break;
            case "logo":
                Crest(dc, w / 2, h * 0.36, h * 0.66);
                Name(dc, w / 2, h * 0.84, h * 0.20, true);
                break;
            default:   // (the icon)
                Crest(dc, w / 2, h / 2, w * 0.96);
                break;
        }
    }

    static RadialGradientBrush Radial(Point centre, double radius, params (Color c, double at)[] stops)
    {
        var b = new RadialGradientBrush { MappingMode = BrushMappingMode.Absolute, Center = centre, GradientOrigin = centre, RadiusX = radius, RadiusY = radius };
        foreach (var (c, at) in stops) b.GradientStops.Add(new GradientStop(c, at));
        b.Freeze();
        return b;
    }

    /// The hall: dark, a light from above, the gold light around the crest's place with the lattice in it, the corners darker
    static void Hall(DrawingContext dc, double w, double h, double x, double y, double reach)
    {
        var all = new Rect(0, 0, w, h);
        dc.PushClip(new RectangleGeometry(all));
        dc.DrawRectangle(Fx.Paint(Fx.Ink900), null, all);
        var top = new LinearGradientBrush(Fx.Rgb(0xFFECAA, 0.05), Fx.Rgb(0xFFECAA, 0), 90);
        top.Freeze();
        dc.DrawRectangle(top, null, new Rect(0, 0, w, h * 0.45));
        var centre = new Point(x, y);
        dc.DrawRectangle(Radial(centre, reach, (Fx.Rgb(Fx.Gold500, 0.26), 0), (Fx.Rgb(Fx.Gold500, 0.08), 0.45), (Fx.Rgb(Fx.Gold500, 0), 1)), null, all);
        dc.PushOpacityMask(Radial(centre, reach, (Colors.White, 0), (Color.FromArgb(120, 255, 255, 255), 0.5), (Colors.Transparent, 0.95)));
        dc.DrawRectangle(Art.Lattice(Math.Max(28, reach / 6.4), Fx.Rgb(Fx.Gold500, 0.07)), null, all);
        dc.Pop();
        var far = Math.Sqrt(w * w + h * h) / 2;
        // (black without any of it, not Colors.Transparent: that one is white, and on its way to black it greyed the corners)
        var clear = Color.FromArgb(0, 0, 0, 0);
        dc.DrawRectangle(Radial(new Point(w / 2, h / 2), far, (clear, 0), (clear, 0.55), (Color.FromArgb(150, 0, 0, 0), 1)), null, all);
        dc.Pop();
    }

    /// The crest as painted, so wide, around a point
    static void Crest(DrawingContext dc, double x, double y, double width)
    {
        var height = width * Art.Crest.PixelHeight / Art.Crest.PixelWidth;
        dc.DrawImage(Art.Crest, new Rect(x - width / 2, y - height / 2, width, height));
    }

    /// A hairline of gold that fades to both sides, a small diamond in its middle
    static void Rule(DrawingContext dc, double x, double y, double width)
    {
        var line = new LinearGradientBrush();
        line.GradientStops.Add(new GradientStop(Fx.Rgb(Fx.Gold500, 0), 0));
        line.GradientStops.Add(new GradientStop(Fx.Rgb(Fx.Gold500, 0.7), 0.5));
        line.GradientStops.Add(new GradientStop(Fx.Rgb(Fx.Gold500, 0), 1));
        line.Freeze();
        dc.DrawRectangle(line, null, new Rect(x - width / 2, y - 0.75, width, 1.5));
        dc.PushTransform(new RotateTransform(45, x, y));
        dc.DrawRectangle(Fx.Paint(Fx.Gold300), null, new Rect(x - 4.5, y - 4.5, 9, 9));
        dc.Pop();
    }

    /// The name in the window's own serif, capitals set apart, its middle at a point. On nothing (the logo) it gets a soft dark
    /// edge, since Steam lays it over whatever the background is
    static void Name(DrawingContext dc, double x, double y, double size, bool onNothing)
    {
        FormattedText Words(Brush brush) =>
            new(Fx.Track(Program.AppName.ToUpperInvariant(), 3), CultureInfo.InvariantCulture, FlowDirection.LeftToRight, Serif, size, brush, 1.0);
        var words = Words(Fx.Paint(Fx.TextHi));
        var at = new Point(x - words.WidthIncludingTrailingWhitespace / 2, y - words.Height / 2);
        if (onNothing)
        {
            var edge = words.BuildGeometry(at);
            var pen = new Pen(Fx.Paint(0x000000, 0.55), size * 0.09) { LineJoin = PenLineJoin.Round };
            pen.Freeze();
            dc.DrawGeometry(null, pen, edge);
        }
        dc.DrawText(words, at);
    }
}
